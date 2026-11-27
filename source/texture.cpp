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
        // Match texture_inflight_decode_limit: enough prepare threads and compute
        // slots that scrolling can use CPU headroom to keep the GPU queue dense.
        config.max_inflight_compute = 12;
        config.prepare_workers = 8;
        config.max_concurrent_cpu = 4;
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

        // Dropping the tasks cancels their decodes and queues their textures.
        m_cache.clear();
        m_pinned.clear();
        m_pin_set.clear();
        clearPartialReads();

        // Fires the remaining callbacks (Cancelled); their images are freed there.
        m_decoder.reset();

        while (!m_gpu_destroy_queue.empty())
        {
            m_renderer.destroyTexture(m_gpu_destroy_queue.front());
            m_gpu_destroy_queue.pop_front();
        }

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
    // the UI thread.
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
            std::lock_guard lock(m_gpu_destroy_mutex);
            m_gpu_destroy_queue.push_back(texture);
        }

        delete task;
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

        if (trace_decode)
        {
            printReport(task->name, result);
        }
    }

    void TextureCache::drainGpuDestroys()
    {
        // Attempt each currently-queued handle once: tryDestroyTexture refuses
        // textures an in-flight frame still references, which go to the back.
        size_t remaining;
        {
            std::lock_guard lock(m_gpu_destroy_mutex);
            remaining = m_gpu_destroy_queue.size();
        }

        for (; remaining > 0; --remaining)
        {
            TextureHandle handle = 0;

            {
                std::lock_guard lock(m_gpu_destroy_mutex);
                if (m_gpu_destroy_queue.empty())
                {
                    return;
                }

                handle = m_gpu_destroy_queue.front();
                m_gpu_destroy_queue.pop_front();
            }

            if (!m_renderer.tryDestroyTexture(handle))
            {
                std::lock_guard lock(m_gpu_destroy_mutex);
                m_gpu_destroy_queue.push_back(handle);
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
        const size_t limit = texture_prefetch_size + 1;
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

        // Erasing drops the last reference; the deleter cancels the decode.
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
        const size_t keep = texture_prefetch_size;

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
            m_cache.insert(index, task);
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
        for (size_t i = 0; i < texture_prefetch_size; ++i)
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
                m_cache.insert(it->first, it->second);
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
        if (isAborting() || !m_prefetch_direction || texture_prefetch_size == 0)
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
        if (countActiveDecodes() >= texture_inflight_decode_limit)
        {
            return;
        }

        const size_t count = m_indexer.size();
        if (!count)
        {
            return;
        }

        for (size_t i = 0; i < texture_prefetch_size; ++i)
        {
            const size_t index = modulo(priority_index + (i + 1) * size_t(m_prefetch_direction), count);

            if (lookupTask(index))
            {
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

    std::shared_ptr<DecodeTask> TextureCache::getTexture(size_t index, bool priority)
    {
        // Navigation defines a new pin window (current image + prefetch window).
        // Establish it before lookup/creation so the visible image is routed to the
        // eviction-immune overlay, then abort every other in-flight read/decode so
        // this file starts immediately.
        if (priority)
        {
            repin(index);
            abortNonPriorityWork(index);
        }

        auto entry = lookupTask(index);
        if (entry)
        {
            return entry;
        }

        if (index >= m_indexer.size())
        {
            return {};
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

        drainGpuDestroys();

        m_completed = 0;
        m_decoder->process();

        repin(priority_index);
        cancelStaleDecodes(priority_index);
        tickPrefetch(priority_index);

        forEachTask([this] (size_t /*index*/, std::shared_ptr<DecodeTask>& task)
        {
            promoteHeader(*task);
        });

        if (priority_task)
        {
            promoteHeader(*priority_task);
        }

        return m_completed > 0;
    }

    void TextureCache::wait()
    {
        if (m_decoder)
            m_decoder->wait();
    }

} // namespace ifap
