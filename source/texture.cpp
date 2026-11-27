/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2026 Twilight 3D Finland Oy. All rights reserved.
*/
#include "texture.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace ifap
{

    std::recursive_mutex filesystem_mutex;

    // Flip to false to silence the per-image decode report.
    static constexpr bool trace_decode = true;

    using mango::vulkan::TextureDecoder;
    using mango::vulkan::TextureDecoderConfig;
    using mango::vulkan::TextureDecodePath;
    using mango::vulkan::TextureResult;
    using mango::vulkan::TextureStatus;

    namespace
    {

        PixelFormat toPixelFormat(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8G8B8A8_SRGB:       return PixelFormat::RGBA8_SRGB;
                case VK_FORMAT_R16G16B16A16_SFLOAT: return PixelFormat::RGBA16F;
                case VK_FORMAT_R32G32B32A32_SFLOAT: return PixelFormat::RGBA32F;
                default:                            return PixelFormat::RGBA8_UNORM;
            }
        }

        void printReport(const std::string& name, const TextureResult& result)
        {
            const auto& info = result.info;
            const auto& r = result.report;

            std::string path(mango::vulkan::getString(r.path));
            if (!r.fallback_reason.empty())
            {
                path += fmt::format(" ({})", r.fallback_reason);
            }

            std::string size = fmt::format("{} x {}", info.source_width, info.source_height);
            if (r.downscaled)
            {
                size += fmt::format(" -> {} x {}", info.width, info.height);
            }

            const double mp = double(info.source_width) * double(info.source_height) / 1e6;
            const double mps = r.ready_ms > 0.0 ? mp / (r.ready_ms / 1000.0) : 0.0;
            printLine(Print::Info, "[decode] {}: {}, {}, {}, ready {:.1f} ms ({:.0f} MP/s), visible {:.1f} ms",
                name, size, path, r.color_source, r.ready_ms, mps, r.visible_ms);
        }

    } // namespace

    // -----------------------------------------------------------------------
    // TextureCache
    // -----------------------------------------------------------------------

    TextureCache::TextureCache(VKRenderer& renderer,
                               std::function<void()> on_content_changed,
                               std::function<bool()> should_abort)
        : m_renderer(renderer)
        , m_on_content_changed(std::move(on_content_changed))
        , m_should_abort(std::move(should_abort))
    {
        static const u8 placeholder_pixel[] = { 32, 32, 32, 255 };
        m_placeholder = m_renderer.createTexture(1, 1, PixelFormat::RGBA8_UNORM, placeholder_pixel);

        TextureDecoderConfig config;
        // on_content_changed is AppView::requestRedraw → invalidate + schedule.
        config.wake = m_on_content_changed;
        // Decoder allocates the VkImage only at record/submit. Still keep slots
        // low so extreme/ cannot stack many full-frame GPU images + staging.
        config.max_inflight_compute = 2;
        config.prepare_workers = 2;
        config.max_concurrent_cpu = 1;
        m_decoder = m_renderer.createTextureDecoder(config);

        m_worker = std::thread([this] { workerThreadMain(); });
    }

    TextureCache::~TextureCache()
    {
        shutdown();

        {
            std::lock_guard lock(m_worker_mutex);
            m_worker_running = false;
            m_worker_jobs.clear();
        }
        m_worker_cv.notify_all();
        m_worker.join();

        // Drop every shared_ptr while this object (and its mutexes) are still alive.
        // m_display_holdout is declared before m_gpu_destroy_mutex; if it were left to
        // member-destruction order, releaseTask would lock a destroyed mutex on exit.
        m_display_holdout.reset();
        m_cache.clear();
        m_pinned.clear();
        m_pin_set.clear();
        clearPartialReads();

        // Fires the remaining callbacks (Cancelled); their images are freed there.
        m_decoder.reset();

        while (!m_gpu_destroy_queue.empty())
        {
            m_renderer.destroyTexture(m_gpu_destroy_queue.front().handle);
            m_gpu_destroy_queue.pop_front();
        }
        m_pending_destroy_bytes.store(0, std::memory_order_relaxed);

        if (m_placeholder)
        {
            m_renderer.destroyTexture(m_placeholder);
            m_placeholder = 0;
        }
    }

    TextureCache::operator const ImageFileIndexer& () const
    {
        return m_indexer;
    }

    bool TextureCache::isAborting() const
    {
        return m_shutdown || (m_should_abort && m_should_abort());
    }

    void TextureCache::shutdown()
    {
        if (m_shutdown.exchange(true))
        {
            return;
        }

        m_indexer.stop();

        forEachTask([this] (size_t /*index*/, std::shared_ptr<DecodeTask>& task)
        {
            if (const u64 handle = task ? task->decode_handle.load() : 0)
            {
                m_decoder->cancel(handle);
            }
        });

        m_worker_cv.notify_all();
    }

    std::shared_ptr<DecodeTask> TextureCache::makeTask()
    {
        return std::shared_ptr<DecodeTask>(new DecodeTask(), [this] (DecodeTask* task)
        {
            releaseTask(task);
        });
    }

    // Runs on whichever thread drops the last reference: cancels the decode (its
    // callback still fires and frees the image) and defers the texture destroy to
    // the UI thread. UI-thread eviction prefers reclaimTaskGpu() so the handle is
    // already cleared and this path is a no-op for the VkImage.
    void TextureCache::releaseTask(DecodeTask* task)
    {
        if (!task)
        {
            return;
        }

        const u64 handle = task->decode_handle.load();
        if (handle && m_decoder)
        {
            m_decoder->cancel(handle);
        }

        const TextureHandle texture = task->texture.handle;
        if (texture && texture != m_placeholder)
        {
            size_t bytes = task->image_bytes ? task->image_bytes : task->estimatedBytes();
            if (!bytes)
                bytes = m_typical_image_bytes.load(std::memory_order_relaxed);
            queueGpuDestroy(texture, bytes);
        }

        delete task;
    }

    void TextureCache::queueGpuDestroy(TextureHandle handle, size_t bytes)
    {
        if (!handle || handle == m_placeholder)
            return;

        {
            std::lock_guard lock(m_gpu_destroy_mutex);
            m_gpu_destroy_queue.push_back(PendingDestroy{ handle, bytes });
        }
        if (bytes)
            m_pending_destroy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    void TextureCache::reclaimTaskGpu(DecodeTask& task)
    {
        // Keep the paint holdout's VkImage — findDisplayTask may still be showing it.
        if (m_display_holdout && m_display_holdout.get() == &task)
            return;

        const TextureHandle handle = task.texture.handle;
        if (!handle || handle == m_placeholder)
            return;

        // Detach so releaseTask / a later shared_ptr drop does not queue again.
        task.texture.handle = 0;
        m_renderer.destroyTexture(handle);
    }

    void TextureCache::enqueuePrepare(std::shared_ptr<DecodeTask> task, bool front)
    {
        {
            std::lock_guard lock(m_worker_mutex);

            // Navigation (the visible image) jumps the queue so a fast scroll always
            // prepares the latest target first; prefetch stays FIFO at the back.
            if (front)
            {
                m_worker_jobs.push_front(std::move(task));
            }
            else
            {
                m_worker_jobs.push_back(std::move(task));
            }
        }

        m_worker_cv.notify_one();
    }

    void TextureCache::workerThreadMain()
    {
        for (;;)
        {
            std::shared_ptr<DecodeTask> task;

            {
                std::unique_lock lock(m_worker_mutex);

                m_worker_cv.wait(lock, [this]
                {
                    return !m_worker_running || !m_worker_jobs.empty();
                });

                if (!m_worker_running)
                {
                    return;
                }

                task = std::move(m_worker_jobs.front());
                m_worker_jobs.pop_front();
            }

            if (!m_shutdown)
            {
                runPrepare(task);
            }
        }
    }

    void TextureCache::runPrepare(const std::shared_ptr<DecodeTask>& task)
    {
        if (!task || isAborting())
        {
            return;
        }

        // Evicted before we reached it: only this job still references the task.
        if (task.use_count() <= 1)
        {
            return;
        }

        std::shared_ptr<Buffer> data;

        try
        {
            // Bulk, sequential read of the whole compressed file into RAM. Going through
            // File(path, name) preserves the custom-mapper / archive support. The copy
            // runs in blocks with an abort check between them: if the user scrolled past
            // this image, we bail and stash the prefix so a later prepare can resume.
            std::lock_guard lock(filesystem_mutex);

            if (!task->path)
            {
                return;
            }

            File file(*task->path, task->name);
            ConstMemory src = file;

            // Huge compressed JPEGs are almost always huge RGBA. Bump the folder
            // typical before header so prefetch clamps before the first 16k lands
            // (mix folders otherwise ramp on small-image ceilings).
            if (src.size >= (16ull << 20))
                observeImageBytes(1024ull * 1024ull * 1024ull);

            PartialFileRead partial = takePartialRead(task->index);
            std::unique_ptr<Buffer> buffer;
            size_t offset = 0;

            if (partial.buffer && partial.buffer->size() == src.size &&
                partial.bytes > 0 && partial.bytes <= src.size)
            {
                buffer = std::move(partial.buffer);
                offset = partial.bytes;
            }
            else
            {
                buffer = std::make_unique<Buffer>(src.size);
            }

            u8* dst = buffer->data();

            for (; offset < src.size; offset += texture_read_block_size)
            {
                if (isAborting() || task.use_count() <= 1)
                {
                    stashPartialRead(task->index, std::move(buffer), offset);
                    return;
                }

                const size_t block = std::min(texture_read_block_size, src.size - offset);
                std::memcpy(dst + offset, src.address + offset, block);
            }

            data = std::move(buffer);
        }
        catch (const std::exception& e)
        {
            printLine(Print::Error, "{}: {}", task->name, e.what());
            task->prepare_state = PrepareState::Failed;
            if (m_on_content_changed)
            {
                m_on_content_changed();
            }
            return;
        }

        // Refuse prefetch when resident + pending destroys already fill the budget.
        // (VkImage is deferred until GPU record, but Ready textures + file RAM still count.)
        const size_t need = task->estimatedBytes()
            ? task->estimatedBytes()
            : (data->size() >= (16ull << 20)
                ? size_t(1024ull * 1024ull * 1024ull)
                : m_typical_image_bytes.load(std::memory_order_relaxed));
        if (!task->priority && need &&
            pressureBytes() + need > texture_memory_budget_bytes)
        {
            // Back to Pending so tickPrefetch can retry once destroys catch up.
            task->prepare_state = PrepareState::Pending;
            if (m_on_content_changed)
                m_on_content_changed();
            return;
        }

        // The callback holds the file memory until the decode is done with it.
        std::weak_ptr<DecodeTask> weak = task;
        TextureDecoder::Texture texture = m_decoder->decode(*data, task->name,
            [this, weak, data] (const TextureResult& result)
            {
                onDecoded(weak, result);
            });

        if (!texture)
        {
            printLine(Print::Error, "{}: {}", task->name, texture.error);
            task->prepare_state = PrepareState::Failed;
        }
        else
        {
            task->decode_handle = texture.handle;
            task->header_width = int(texture.info.source_width);
            task->header_height = int(texture.info.source_height);
            task->header_ready.store(true);

            if (isAborting())
            {
                m_decoder->cancel(texture.handle);
            }
        }

        if (m_on_content_changed)
        {
            m_on_content_changed();
        }
    }

    // UI thread, from m_decoder->process().
    void TextureCache::onDecoded(const std::weak_ptr<DecodeTask>& weak, const TextureResult& result)
    {
        ++m_completed;

        std::shared_ptr<DecodeTask> task = weak.lock();
        if (task)
        {
            task->decode_handle = 0;
        }

        if (!task || result.status != TextureStatus::Ready)
        {
            m_renderer.destroyImage(result.image);

            if (task)
            {
                task->prepare_state = PrepareState::Failed;
                if (result.status == TextureStatus::Failed)
                {
                    printLine(Print::Error, "{}: {}", task->name, result.error);
                }
            }
            return;
        }

        const TextureHandle handle = m_renderer.adoptTexture(result.image, result.info);
        if (!handle)
        {
            printLine(Print::Error, "{}: texture view creation failed", task->name);
            task->prepare_state = PrepareState::Failed;
            return;
        }

        GpuTexture& texture = task->texture;
        texture.handle = handle;
        texture.width = int(result.info.source_width);
        texture.height = int(result.info.source_height);
        texture.sample_width = int(result.info.width);
        texture.sample_height = int(result.info.height);
        texture.format = toPixelFormat(result.info.format);
        texture.linear = result.info.linear;
        texture.needs_tonemap = result.info.needs_tonemap;

        task->gpu_texture_ready = true;
        task->present_settle_frames = 2;
        task->prepare_state = PrepareState::Ready;
        task->image_bytes = result.report.image_bytes
            ? result.report.image_bytes
            : size_t(result.info.width) * size_t(result.info.height) * 4;
        observeImageBytes(task->image_bytes);
        refreshCacheCost(task->index, task);
        enforceMemoryBudget(task->index);

        if (trace_decode)
        {
            printReport(task->name, result);
        }
    }

    void TextureCache::drainGpuDestroys(bool force)
    {
        // Under memory pressure, block on the timeline so VkImages actually return
        // instead of accumulating in this queue while residentBytes() looks fine.
        const bool pressure = force
            || pressureBytes() > texture_memory_budget_bytes
            || m_pending_destroy_bytes.load(std::memory_order_relaxed) > (texture_memory_budget_bytes / 4);

        size_t remaining;
        {
            std::lock_guard lock(m_gpu_destroy_mutex);
            remaining = m_gpu_destroy_queue.size();
        }

        for (; remaining > 0; --remaining)
        {
            PendingDestroy pending;

            {
                std::lock_guard lock(m_gpu_destroy_mutex);
                if (m_gpu_destroy_queue.empty())
                    return;

                pending = m_gpu_destroy_queue.front();
                m_gpu_destroy_queue.pop_front();
            }

            bool freed = m_renderer.tryDestroyTexture(pending.handle);
            if (!freed && pressure)
            {
                m_renderer.destroyTexture(pending.handle);
                freed = true;
            }

            if (freed)
            {
                if (pending.bytes)
                    m_pending_destroy_bytes.fetch_sub(
                        std::min(pending.bytes, m_pending_destroy_bytes.load(std::memory_order_relaxed)),
                        std::memory_order_relaxed);
            }
            else
            {
                std::lock_guard lock(m_gpu_destroy_mutex);
                m_gpu_destroy_queue.push_back(pending);
            }
        }
    }

    // Until the pixels arrive the placeholder is drawn with the image's aspect.
    void TextureCache::promoteHeader(DecodeTask& task)
    {
        if (task.gpu_texture_ready || !task.header_ready.load())
        {
            return;
        }

        task.texture.width = task.header_width;
        task.texture.height = task.header_height;
        observeImageBytes(task.estimatedBytes());
    }

    size_t TextureCache::prefetchLimit() const
    {
        const size_t typical = m_typical_image_bytes.load(std::memory_order_relaxed);
        if (typical == 0)
            return texture_prefetch_size;

        // How many full frames fit in the budget; keep at least current + one ahead.
        const size_t max_frames = std::max(size_t(2), texture_memory_budget_bytes / typical);
        size_t limit = std::min(texture_prefetch_size, max_frames - 1);
        // ≥256 MB/frame: at most one ahead — otherwise scroll outruns reclaim.
        if (typical >= (256ull << 20))
            limit = std::min(limit, size_t(1));
        return limit;
    }

    size_t TextureCache::inflightLimit() const
    {
        // Large frames: keep concurrent decodes ~half the resident window so
        // in-flight VkImages and already-Ready pins do not both sit at the budget.
        const size_t typical = m_typical_image_bytes.load(std::memory_order_relaxed);
        if (typical == 0)
            return texture_inflight_decode_limit;

        const size_t by_budget = std::max(size_t(1),
            (texture_memory_budget_bytes / typical + 1) / 2);
        size_t limit = std::min({ texture_inflight_decode_limit, prefetchLimit() + 1, by_budget });
        if (typical >= (256ull << 20))
            limit = std::min(limit, size_t(2));
        return limit;
    }

    size_t TextureCache::taskCost(const DecodeTask& task) const
    {
        if (const size_t bytes = task.estimatedBytes())
            return bytes;
        // Unknown footprint (decode not headed yet): reserve the folder worst-case
        // so a pending 16k does not look free next to cheap hackatron entries.
        if (const size_t typical = m_typical_image_bytes.load(std::memory_order_relaxed))
            return typical;
        return 1;
    }

    size_t TextureCache::residentBytes() const
    {
        // Sum live task footprints — do NOT trust CostLRUCache::used(). Entries are
        // often inserted at cost 1 / small-header size before Ready, and the book
        // was never updated; that let ~50×1 GB PIAs sit under an "8 GiB" budget.
        size_t bytes = 0;
        for (const auto& entry : m_pinned)
        {
            if (entry.second)
                bytes += taskCost(*entry.second);
        }
        m_cache.for_each([&] (size_t /*index*/, const std::shared_ptr<DecodeTask>& task)
        {
            if (task)
                bytes += taskCost(*task);
        });
        // Holdout may sit outside pin/cache after eviction; still counts toward budget.
        if (m_display_holdout)
        {
            auto still = peekTask(m_display_holdout->index);
            if (still.get() != m_display_holdout.get())
                bytes += taskCost(*m_display_holdout);
        }
        return bytes;
    }

    size_t TextureCache::pendingDestroyBytes() const
    {
        return m_pending_destroy_bytes.load(std::memory_order_relaxed);
    }

    size_t TextureCache::pressureBytes() const
    {
        return residentBytes() + pendingDestroyBytes();
    }

    void TextureCache::observeImageBytes(size_t bytes)
    {
        if (!bytes)
            return;

        size_t cur = m_typical_image_bytes.load(std::memory_order_relaxed);
        while (bytes > cur &&
               !m_typical_image_bytes.compare_exchange_weak(
                   cur, bytes, std::memory_order_relaxed))
        {
        }
    }

    void TextureCache::refreshCacheCost(size_t index, const std::shared_ptr<DecodeTask>& task)
    {
        if (!task || m_pinned.find(index) != m_pinned.end())
            return;

        // Re-insert so the cost ledger matches the real VkImage. get() no-ops when
        // absent (task already dropped) and promotes when present (just-finished = hot).
        if (m_cache.get(index))
            m_cache.insert(index, task, taskCost(*task));
    }

    void TextureCache::enforceMemoryBudget(size_t keep_index)
    {
        auto over = [this] {
            return pressureBytes() > texture_memory_budget_bytes;
        };

        // Evictable store first. Prefer dropping unfinished work; keep Ready trail
        // frames for findDisplayTask until we truly need the bytes (large-frame folders).
        auto evictOne = [&] () -> bool
        {
            if (m_cache.size() == 0)
                return false;

            // Pass 1: non-Ready LRU.
            for (auto it = m_cache.end(); it != m_cache.begin(); )
            {
                --it;
                if (!it->value)
                {
                    m_cache.erase(it->key);
                    return true;
                }
                if (it->value->prepare_state.load() != PrepareState::Ready)
                {
                    reclaimTaskGpu(*it->value);
                    m_cache.erase(it->key);
                    return true;
                }
            }

            // Pass 2: Ready LRU (large-frame pressure only).
            auto victim = std::prev(m_cache.end());
            const size_t key = victim->key;
            if (victim->value)
                reclaimTaskGpu(*victim->value);
            m_cache.erase(key);
            return true;
        };

        while (over() && evictOne())
        {
        }

        // Pin window can itself exceed the budget if it was built before typical
        // grew (mix: small headers → wide prefetch, then 16k frames fill it). Drop
        // prefetch-tail pins, never the keep/visible index.
        while (over() && m_pinned.size() > 1)
        {
            size_t victim = size_t(-1);
            for (auto it = m_pin_set.rbegin(); it != m_pin_set.rend(); ++it)
            {
                if (*it != keep_index && m_pinned.find(*it) != m_pinned.end())
                {
                    victim = *it;
                    break;
                }
            }
            if (victim == size_t(-1))
                break;

            if (auto it = m_pinned.find(victim); it != m_pinned.end() && it->second)
                reclaimTaskGpu(*it->second);
            m_pinned.erase(victim);
            m_pin_set.erase(std::remove(m_pin_set.begin(), m_pin_set.end(), victim), m_pin_set.end());
        }

        if (over())
            drainGpuDestroys(true);
    }

    void TextureCache::setPrefetchDirection(int direction)
    {
        m_prefetch_direction = direction;
    }

    void TextureCache::stashPartialRead(size_t index, std::unique_ptr<Buffer> buffer, size_t bytes)
    {
        if (!buffer || bytes == 0)
        {
            return;
        }

        std::lock_guard lock(m_partial_mutex);

        m_partial_reads[index] = PartialFileRead{ std::move(buffer), bytes };

        // Bound retained prefixes so a fast scroll across huge files cannot pin an
        // unbounded amount of host RAM. Keep the entry we just stashed; drop others
        // until we are within the prefetch window (+ the visible image).
        const size_t limit = prefetchLimit() + 1;
        while (m_partial_reads.size() > limit)
        {
            bool erased = false;
            for (auto it = m_partial_reads.begin(); it != m_partial_reads.end(); ++it)
            {
                if (it->first != index)
                {
                    m_partial_reads.erase(it);
                    erased = true;
                    break;
                }
            }

            if (!erased)
            {
                break;
            }
        }
    }

    TextureCache::PartialFileRead TextureCache::takePartialRead(size_t index)
    {
        std::lock_guard lock(m_partial_mutex);

        auto it = m_partial_reads.find(index);
        if (it == m_partial_reads.end())
        {
            return {};
        }

        PartialFileRead partial = std::move(it->second);
        m_partial_reads.erase(it);
        return partial;
    }

    void TextureCache::clearPartialReads()
    {
        std::lock_guard lock(m_partial_mutex);
        m_partial_reads.clear();
    }

    void TextureCache::abortNonPriorityWork(size_t priority_index)
    {
        // Navigation just settled on (or jumped to) priority_index. Drop every other
        // in-flight read/decode so the read worker and the decoder are free for the
        // visible image. Finished neighbors stay resident for instant back-navigation;
        // tickPrefetch() rebuilds the window once the current file is underway.
        std::vector<size_t> drop;

        forEachTask([&] (size_t index, std::shared_ptr<DecodeTask>& task)
        {
            if (index != priority_index && task && task->isDecoding())
            {
                drop.push_back(index);
            }
        });

        // Drop references only — these are still Decoding (placeholder texture).
        // Avoid reclaimTaskGpu here; it would block the UI thread on the timeline
        // during hold-to-scroll and starve process()/Ready updates.
        for (size_t index : drop)
        {
            m_pinned.erase(index);
            m_cache.erase(index);
        }

        // Drop queued reads that are no longer wanted.
        {
            std::lock_guard lock(m_worker_mutex);

            std::deque<std::shared_ptr<DecodeTask>> kept;
            for (std::shared_ptr<DecodeTask>& task : m_worker_jobs)
            {
                if (task && task->index == priority_index)
                {
                    kept.push_back(std::move(task));
                }
            }

            m_worker_jobs.swap(kept);
        }
    }

    void TextureCache::cancelStaleDecodes(size_t priority_index)
    {
        const size_t count = m_indexer.size();
        if (!count)
        {
            return;
        }

        // Keep in-flight decodes bounded to a window around the visible image.
        // Finished textures stay cached so navigating back to them is instant.
        const size_t keep = prefetchLimit();

        std::vector<size_t> stale;

        m_cache.for_each([&] (size_t index, std::shared_ptr<DecodeTask>& task)
        {
            if (index == priority_index || !task || !task->isDecoding())
            {
                return;
            }

            const size_t forward = (index + count - priority_index) % count;
            const size_t backward = (priority_index + count - index) % count;
            const size_t distance = std::min(forward, backward);

            if (distance > keep)
            {
                stale.push_back(index);
            }
        });

        for (size_t index : stale)
        {
            m_cache.erase(index);
        }
    }

    bool TextureCache::isPinIndex(size_t index) const
    {
        return std::find(m_pin_set.begin(), m_pin_set.end(), index) != m_pin_set.end();
    }

    std::shared_ptr<DecodeTask> TextureCache::lookupTask(size_t index)
    {
        auto it = m_pinned.find(index);
        if (it != m_pinned.end())
        {
            return it->second;
        }

        if (auto cached = m_cache.get(index))
        {
            return cached.value();
        }

        return {};
    }

    std::shared_ptr<DecodeTask> TextureCache::peekTask(size_t index) const
    {
        auto it = m_pinned.find(index);
        if (it != m_pinned.end())
            return it->second;

        // Const scan — avoid LRU touch on a paint-path lookup.
        std::shared_ptr<DecodeTask> found;
        m_cache.for_each([&] (size_t key, const std::shared_ptr<DecodeTask>& task)
        {
            if (key == index)
                found = task;
        });
        return found;
    }

    std::shared_ptr<DecodeTask> TextureCache::findDisplayTask(size_t index, int scroll_direction) const
    {
        const size_t count = m_indexer.size();
        if (!count)
            return {};

        auto isPaintReady = [this] (const DecodeTask& task) -> bool
        {
            return task.prepare_state.load() == PrepareState::Ready
                && task.gpu_texture_ready
                && task.texture.handle != 0
                && task.texture.handle != m_placeholder;
        };

        // Trail distance from `index` walking opposite the scroll direction.
        const int step = scroll_direction < 0 ? 1 : -1;
        auto trailDistance = [&] (size_t candidate) -> size_t
        {
            for (size_t n = 0; n < count; ++n)
            {
                const size_t idx = size_t(modulo(
                    ptrdiff_t(index) + ptrdiff_t(step) * ptrdiff_t(n),
                    ptrdiff_t(count)));
                if (idx == candidate)
                    return n;
            }
            return count;
        };

        // Prefer the Ready frame closest behind (or at) the live index among
        // everything still resident — not just the first hit / sticky holdout.
        std::shared_ptr<DecodeTask> best;
        size_t best_dist = count;

        auto consider = [&] (const std::shared_ptr<DecodeTask>& task)
        {
            if (!task || !isPaintReady(*task))
                return;
            const size_t dist = trailDistance(task->index);
            if (dist < best_dist)
            {
                best_dist = dist;
                best = task;
            }
        };

        consider(peekTask(index));
        for (const auto& entry : m_pinned)
            consider(entry.second);
        m_cache.for_each([&] (size_t /*key*/, const std::shared_ptr<DecodeTask>& task)
        {
            consider(task);
        });
        consider(m_display_holdout);

        if (best)
        {
            m_display_holdout = best;
            return best;
        }

        return peekTask(index);
    }

    void TextureCache::storeTask(size_t index, const std::shared_ptr<DecodeTask>& task, bool pin_overlay)
    {
        if (pin_overlay)
        {
            m_pinned[index] = task;
            m_cache.erase(index);

            if (std::find(m_pin_set.begin(), m_pin_set.end(), index) == m_pin_set.end())
            {
                m_pin_set.push_back(index);
            }

            return;
        }

        // Window members go into the eviction-immune overlay; everything else into
        // the evictable cache. repin() keeps m_pin_set in sync with the window.
        if (isPinIndex(index))
        {
            m_pinned[index] = task;
        }
        else
        {
            m_cache.insert(index, task, taskCost(*task));
            enforceMemoryBudget(index);
        }
    }

    void TextureCache::forEachTask(const std::function<void(size_t, std::shared_ptr<DecodeTask>&)>& fn)
    {
        for (auto& entry : m_pinned)
        {
            fn(entry.first, entry.second);
        }

        m_cache.for_each(fn);
    }

    void TextureCache::repin(size_t priority_index)
    {
        const size_t count = m_indexer.size();
        if (!count)
        {
            m_pin_set.assign(1, priority_index);
            return;
        }

        // Desired pin window: the current image plus the active prefetch window,
        // built with the same index math as tickPrefetch().
        std::vector<size_t> desired;
        desired.push_back(priority_index % count);

        const int dir = m_prefetch_direction ? m_prefetch_direction : 1;
        const size_t prefetch = prefetchLimit();
        for (size_t i = 0; i < prefetch; ++i)
        {
            const size_t idx = modulo(priority_index + (i + 1) * size_t(dir), count);
            if (std::find(desired.begin(), desired.end(), idx) == desired.end())
            {
                desired.push_back(idx);
            }
        }

        auto inDesired = [&] (size_t idx)
        {
            return std::find(desired.begin(), desired.end(), idx) != desired.end();
        };

        // Migrate out: entries that left the window return to the evictable cache,
        // staying resident so navigating back to them is instant.
        for (auto it = m_pinned.begin(); it != m_pinned.end(); )
        {
            if (!inDesired(it->first))
            {
                m_cache.insert(it->first, it->second, taskCost(*it->second));
                it = m_pinned.erase(it);
            }
            else
            {
                ++it;
            }
        }

        m_pin_set = std::move(desired);

        // Migrate in: window members sitting in the evictable cache are promoted to
        // the overlay so eviction can never drop them.
        for (size_t idx : m_pin_set)
        {
            if (m_pinned.find(idx) != m_pinned.end())
            {
                continue;
            }

            if (auto cached = m_cache.get(idx))
            {
                m_pinned[idx] = cached.value();
                m_cache.erase(idx);
            }
        }

        enforceMemoryBudget(priority_index);
    }

    size_t TextureCache::countActiveDecodes() const
    {
        size_t active = 0;

        for (const auto& entry : m_pinned)
        {
            if (entry.second && entry.second->isDecoding())
            {
                ++active;
            }
        }

        m_cache.for_each([&active] (size_t /*index*/, const std::shared_ptr<DecodeTask>& task)
        {
            if (task && task->isDecoding())
            {
                ++active;
            }
        });

        return active;
    }

    void TextureCache::tickPrefetch(size_t priority_index)
    {
        const size_t prefetch = prefetchLimit();
        if (isAborting() || !m_prefetch_direction || prefetch == 0)
        {
            return;
        }

        // Hold prefetch until the visible image has been handed to the decoder, so
        // its file read never competes with prefetch I/O.
        auto priority_task = lookupTask(priority_index);
        if (!priority_task || (priority_task->isDecoding() && !priority_task->header_ready.load()))
        {
            return;
        }

        // Bound the number of decodes (and their memory) in flight at once.
        if (countActiveDecodes() >= inflightLimit())
        {
            return;
        }

        // Stay under the byte budget before starting another full-resolution decode
        // (include destroy-queue backlog — those VkImages are still alive).
        if (pressureBytes() + taskCost(*priority_task) > texture_memory_budget_bytes)
        {
            return;
        }

        const size_t count = m_indexer.size();
        if (!count)
        {
            return;
        }

        for (size_t i = 0; i < prefetch; ++i)
        {
            const size_t index = modulo(priority_index + (i + 1) * size_t(m_prefetch_direction), count);

            if (auto existing = lookupTask(index))
            {
                // Retry a prefetch that deferred decode() under memory pressure.
                if (existing->prepare_state.load() == PrepareState::Pending)
                {
                    existing->prepare_state = PrepareState::Preparing;
                    enqueuePrepare(existing, false);
                    return;
                }
                continue;
            }

            getTexture(index);
            return;
        }
    }

    size_t TextureCache::setCurrentPath(const std::string& name)
    {
        m_cache.clear();
        m_pinned.clear();
        m_pin_set.clear();
        m_typical_image_bytes.store(0, std::memory_order_relaxed);
        m_display_holdout.reset();
        clearPartialReads();

        std::string filename = name;
        const std::string pathname = getPath(filename);
        const std::string basename = removePath(filename);
        Path temp(pathname);

        // Path::isFile() takes a name relative to the path's directory, not the full path.
        if (!basename.empty() && temp.isFile(basename))
        {
            if (Mapper::isCustomMapper(filename))
            {
                std::string pathname = filename + "/";
                m_current_path = std::make_shared<Path>(pathname);
                m_indexer.start(pathname);
                filename.clear();
            }
            else
            {
                if (!mango::image::isImageDecoder(filename))
                {
                    return -1u;
                }

                m_current_path = std::make_shared<Path>(pathname);
                m_indexer.start(pathname);
                filename = basename;
            }
        }
        else
        {
            std::string pathname = getPath(filename);
            m_current_path = std::make_shared<Path>(pathname);
            m_indexer.start(pathname);
            filename.clear();
        }

        size_t m_current_index = -1u;

        if (filename.empty())
        {
            auto getIndexOfFirstFile = [this] () -> size_t
            {
                bool done = false;
                bool last = false;

                while (!done)
                {
                    if (m_should_abort && m_should_abort())
                    {
                        return -1u;
                    }

                    if (m_indexer.size() > 0)
                    {
                        return 0;
                    }

                    mango::Sleep::ms(50);

                    done = last;
                    last = !m_indexer.isRunning();
                }

                return -1u;
            };

            m_current_index = getIndexOfFirstFile();
        }
        else
        {
            auto getIndexFromFilename = [this] (const std::string& filename) -> size_t
            {
                size_t position = 0;

                bool done = false;
                bool last = false;

                while (!done)
                {
                    if (m_should_abort && m_should_abort())
                    {
                        return -1u;
                    }

                    for ( ; position < m_indexer.size(); ++position)
                    {
                        if (m_indexer[position] == filename)
                        {
                            return position;
                        }
                    }

                    mango::Sleep::ms(5);

                    done = last;
                    last = !m_indexer.isRunning();
                }

                return -1u;
            };

            m_current_index = getIndexFromFilename(filename);
        }

        return m_current_index;
    }

    std::shared_ptr<DecodeTask> TextureCache::getTexture(size_t index, bool priority, bool abort_others)
    {
        // Navigation defines a new pin window (current image + prefetch window).
        // Establish it before lookup/creation so the visible image is routed to the
        // eviction-immune overlay. abort_others cancels sibling prepares so a tap
        // focuses immediately; hold-to-scroll skips that so trail Ready frames land.
        if (priority)
        {
            repin(index);
            if (abort_others)
                abortNonPriorityWork(index);
        }

        auto entry = lookupTask(index);
        if (entry)
        {
            if (priority && entry->prepare_state.load() == PrepareState::Pending)
            {
                entry->priority = true;
                entry->prepare_state = PrepareState::Preparing;
                enqueuePrepare(entry, true);
            }
            return entry;
        }

        if (index >= m_indexer.size())
        {
            return {};
        }

        // Hold-to-scroll without abort: don't start another prepare if we're already
        // at the inflight cap — caller still updates the title index; paint uses
        // findDisplayTask on whatever Ready frames have landed.
        if (priority && !abort_others && countActiveDecodes() >= inflightLimit())
        {
            return findDisplayTask(index, m_prefetch_direction ? m_prefetch_direction : 1);
        }

        std::shared_ptr<DecodeTask> task = makeTask();
        task->name = m_indexer[index];
        task->index = index;
        task->path = m_current_path;

        // The shared 1x1 placeholder stands in until the decode callback adopts the
        // real image; its aspect follows the header once the worker publishes it.
        GpuTexture& texture = task->texture;
        texture.handle = m_placeholder;
        texture.width = 1;
        texture.height = 1;
        texture.sample_width = 1;
        texture.sample_height = 1;
        texture.format = PixelFormat::RGBA8_UNORM;

        task->priority = priority;
        task->prepare_state = PrepareState::Preparing;

        enqueuePrepare(task, priority);
        storeTask(index, task, priority);
        return task;
    }

    bool TextureCache::update(size_t priority_index, const std::shared_ptr<DecodeTask>& priority_task)
    {
        if (isAborting())
        {
            return false;
        }

        drainGpuDestroys(pressureBytes() > texture_memory_budget_bytes);

        m_completed = 0;
        m_decoder->process();

        forEachTask([this] (size_t /*index*/, std::shared_ptr<DecodeTask>& task)
        {
            promoteHeader(*task);
        });

        if (priority_task)
        {
            promoteHeader(*priority_task);
            observeImageBytes(priority_task->estimatedBytes());
        }

        // Observe size before repin/prefetch so a folder of huge frames clamps the
        // window on the same frame the header arrives (avoids the 45 GB ramp).
        repin(priority_index);
        cancelStaleDecodes(priority_index);
        enforceMemoryBudget(priority_index);
        tickPrefetch(priority_index);

        return m_completed > 0;
    }

    void TextureCache::wait()
    {
        if (m_decoder)
            m_decoder->wait();
    }

} // namespace ifap
