/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "texture.hpp"

#include <mango/image/bicubic.hpp>

#include <algorithm>
#include <chrono>
#include <memory>

namespace ifap
{

    // Flip to false to silence the decode lifecycle trace.
    static constexpr bool trace_decode = false;

    namespace
    {
        math::int32x2 computeDownscaleDimensions(int width, int height, int max_dimension)
        {
            if (width <= max_dimension && height <= max_dimension)
            {
                return math::int32x2(width, height);
            }

            const float scale = std::min(float(max_dimension) / float(width),
                                         float(max_dimension) / float(height));

            return math::int32x2(
                std::max(1, int(float(width) * scale)),
                std::max(1, int(float(height) * scale)));
        }

        void createPlaceholderTexture(DecodeTask& task, VKRenderer& renderer, int width, int height)
        {
            static const u8 placeholder_pixel[] = { 32, 32, 32, 255 };

            GpuTexture& texture = task.texture;
            texture.width = width;
            texture.height = height;
            texture.sample_width = 1;
            texture.sample_height = 1;
            // format and linear come from selectPixelFormat() — do not clobber them here
            texture.handle = renderer.createTexture(1, 1, texture.format, placeholder_pixel);
        }

        PixelFormat selectPixelFormat(const ImageHeader& header, bool& linear)
        {
            if (header.format.isFloat())
            {
                linear = true;

                if (header.format.bits <= 64)
                {
                    return PixelFormat::RGBA16F;
                }

                return PixelFormat::RGBA32F;
            }

            linear = header.linear;

            if (header.linear)
            {
                return PixelFormat::RGBA8_UNORM;
            }

            return PixelFormat::RGBA8_SRGB;
        }

        Format selectBitmapFormat(const ImageHeader& header)
        {
            if (header.format.isFloat())
            {
                if (header.format.bits <= 64)
                {
                    return Format(64, Format::FLOAT16, Format::RGBA, 16, 16, 16, 16);
                }

                return Format(128, Format::FLOAT32, Format::RGBA, 32, 32, 32, 32);
            }

            return Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8);
        }

    } // namespace

    // -----------------------------------------------------------------------
    // DecodeTask
    // -----------------------------------------------------------------------

    DecodeTask::DecodeTask(VKRenderer& renderer)
        : renderer(renderer)
    {
    }

    DecodeTask::~DecodeTask()
    {
        if (decoder)
        {
            decoder->cancel();
        }

        if (texture.handle)
        {
            renderer.destroyTexture(texture.handle);
        }
    }

    bool DecodeTask::hasPendingUpdates() const
    {
        std::lock_guard lock(mutex);
        return !updates.empty();
    }

    std::vector<ImageDecodeRect> DecodeTask::getUpdates()
    {
        std::lock_guard lock(mutex);
        std::vector<ImageDecodeRect> temp;
        std::swap(temp, updates);
        return temp;
    }

    bool DecodeTask::isDecoding() const
    {
        if (prepare_state.load() != PrepareState::Ready)
        {
            // Still pending/preparing: the decode either has not launched yet or is
            // about to. Treat as in-flight so it counts against the concurrency cap.
            return prepare_state.load() == PrepareState::Preparing;
        }

        return future.valid() &&
            future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }

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
        m_worker = std::thread([this] { workerThreadMain(); });
        m_reaper = std::thread([this] { reaperThreadMain(); });
    }

    TextureCache::~TextureCache()
    {
        shutdown();

        m_cache.clear();
        m_pinned.clear();
        m_pin_set.clear();

        {
            std::lock_guard lock(m_worker_mutex);
            m_worker_running = false;
        }
        m_worker_cv.notify_all();
        m_worker.join();

        {
            std::lock_guard lock(m_reaper_mutex);
            m_reaper_running = false;
        }
        m_reaper_cv.notify_all();
        m_reaper.join();

        drainGpuDestroys(1024);

        while (!m_gpu_destroy_queue.empty())
        {
            m_renderer.destroyTexture(m_gpu_destroy_queue.front());
            m_gpu_destroy_queue.pop_front();
        }
    }

    TextureCache::operator const ImageFileIndexer& () const
    {
        return m_indexer;
    }

    void TextureCache::shutdown()
    {
        if (m_shutdown.exchange(true))
        {
            return;
        }

        m_indexer.stop();

        forEachTask([] (size_t /*index*/, std::shared_ptr<DecodeTask>& task)
        {
            if (task && task->decoder)
            {
                task->decoder->cancel();
            }
        });

        {
            std::lock_guard lock(m_worker_mutex);

            for (WorkerJob& job : m_worker_jobs)
            {
                if (job.task && job.task->decoder)
                {
                    job.task->decoder->cancel();
                }
            }
        }

        // Cancel decoders of queued disposals up front so the reaper's future join
        // returns immediately instead of waiting out a full decode during teardown.
        {
            std::lock_guard lock(m_reaper_mutex);

            for (WorkerJob& job : m_reaper_jobs)
            {
                if (job.task && job.task->decoder)
                {
                    job.task->decoder->cancel();
                }
            }
        }

        m_worker_cv.notify_all();
        m_reaper_cv.notify_all();
    }

    std::shared_ptr<DecodeTask> TextureCache::makeTask()
    {
        return std::shared_ptr<DecodeTask>(new DecodeTask(m_renderer),
            [this] (DecodeTask* task)
            {
                deferDispose(task);
            });
    }

    void TextureCache::deferDispose(DecodeTask* raw_task)
    {
        if (!raw_task)
        {
            return;
        }

        WorkerJob job;
        job.type = WorkerJob::Type::Dispose;
        job.gpu_handle = raw_task->texture.handle;
        raw_task->texture.handle = 0;
        job.task = std::shared_ptr<DecodeTask>(raw_task, [](DecodeTask*) {});
        enqueueDispose(std::move(job));
    }

    void TextureCache::enqueuePrepare(WorkerJob job, bool front)
    {
        {
            std::lock_guard lock(m_worker_mutex);

            // Navigation (the visible image) jumps the queue so a fast scroll always
            // prepares the latest target first; prefetch stays FIFO at the back.
            if (front)
            {
                m_worker_jobs.push_front(std::move(job));
            }
            else
            {
                m_worker_jobs.push_back(std::move(job));
            }
        }

        m_worker_cv.notify_one();
    }

    void TextureCache::enqueueDispose(WorkerJob job)
    {
        {
            std::lock_guard lock(m_reaper_mutex);
            m_reaper_jobs.push_back(std::move(job));
        }

        m_reaper_cv.notify_one();
    }

    void TextureCache::workerThreadMain()
    {
        // Prepare lane only: allocate target bitmaps and launch the async decode.
        // Each job returns quickly (the decode itself runs on its own std::async
        // thread), so navigation jobs queued here are picked up promptly.
        for (;;)
        {
            WorkerJob job;

            {
                std::unique_lock lock(m_worker_mutex);

                m_worker_cv.wait(lock, [this]
                {
                    return !m_worker_running || !m_worker_jobs.empty();
                });

                if (!m_worker_running && m_worker_jobs.empty())
                {
                    return;
                }

                job = std::move(m_worker_jobs.front());
                m_worker_jobs.pop_front();
            }

            if (m_shutdown)
            {
                // Drop pending prepares during teardown; cancel just in case the
                // decoder somehow already launched.
                if (job.task && job.task->decoder)
                {
                    job.task->decoder->cancel();
                }

                continue;
            }

            runPrepare(job.task);
        }
    }

    void TextureCache::reaperThreadMain()
    {
        // Disposal lane only: joining the decode future can block until the decode
        // finishes (or honours cancellation). Isolating it here guarantees a blocking
        // join never stalls prepare/navigation on the worker thread.
        for (;;)
        {
            WorkerJob job;

            {
                std::unique_lock lock(m_reaper_mutex);

                m_reaper_cv.wait(lock, [this]
                {
                    return !m_reaper_running || !m_reaper_jobs.empty();
                });

                if (!m_reaper_running && m_reaper_jobs.empty())
                {
                    return;
                }

                job = std::move(m_reaper_jobs.front());
                m_reaper_jobs.pop_front();
            }

            runDispose(std::move(job));
        }
    }

    void TextureCache::runPrepare(const std::shared_ptr<DecodeTask>& task)
    {
        if (!task || !task->decoder || m_shutdown || (m_should_abort && m_should_abort()))
        {
            return;
        }

        // Evicted before we reached it: the user scrolled past while this prepare sat
        // in the queue, so only this job still references the task. Skip the expensive
        // bitmap allocation + decode launch instead of doing work nobody will use.
        if (task.use_count() <= 1)
        {
            if (trace_decode)
            {
                printLine("[trace] #{} skip-orphan (use_count={})", task->index, task.use_count());
            }
            return;
        }

        try
        {
            if (task->downscale)
            {
                task->scaled_bitmap = std::make_unique<Bitmap>(
                    task->downscale_width, task->downscale_height, task->bitmap_format);
            }

            task->bitmap = std::make_unique<Bitmap>(
                task->texture.width, task->texture.height, task->bitmap_format);

            task->decode_start_ms.store(mango::Time::ms());

            if (trace_decode)
            {
                printLine("[trace] #{} launch", task->index);
            }

            task->future = task->decoder->launch([this, task = task.get()] (const ImageDecodeRect& rect)
            {
                if (m_shutdown || (m_should_abort && m_should_abort()))
                {
                    return;
                }

                bool first = false;

                {
                    std::lock_guard lock(task->mutex);
                    if (!task->decode_first_ms.load())
                    {
                        task->decode_first_ms.store(mango::Time::ms());
                        first = true;
                    }
                    task->updates.push_back(rect);
                    task->progress += rect.progress;
                }

                if (trace_decode && first)
                {
                    printLine("[trace] #{} first-pixels", task->index);
                }

                if (m_on_content_changed)
                {
                    m_on_content_changed();
                }
            }, *task->bitmap);

            if (m_shutdown || (m_should_abort && m_should_abort()))
            {
                task->decoder->cancel();
                return;
            }

            task->prepare_state = PrepareState::Ready;

            if (m_on_content_changed)
            {
                m_on_content_changed();
            }
        }
        catch (...)
        {
            task->prepare_state = PrepareState::Failed;
        }
    }

    void TextureCache::runDispose(WorkerJob job)
    {
        DecodeTask* raw = job.task.get();

        if (raw)
        {
            if (raw->decoder)
            {
                raw->decoder->cancel();
            }

            raw->future = ImageDecodeFuture();
            raw->bitmap.reset();
            raw->scaled_bitmap.reset();
            raw->decoder.reset();
            raw->file.reset();
        }

        job.task.reset();
        delete raw;

        if (job.gpu_handle)
        {
            std::lock_guard lock(m_gpu_destroy_mutex);
            m_gpu_destroy_queue.push_back(job.gpu_handle);
        }
    }

    void TextureCache::drainGpuDestroys(int budget)
    {
        while (budget-- > 0)
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

            // Non-blocking: if uploads are still in flight, requeue for a later frame
            // rather than stalling the main thread on a fence.
            if (!m_renderer.tryDestroyTexture(handle))
            {
                std::lock_guard lock(m_gpu_destroy_mutex);
                m_gpu_destroy_queue.push_back(handle);
            }
        }
    }

    bool TextureCache::finishGpuSetup(DecodeTask& task)
    {
        if (task.gpu_texture_ready || task.downscale)
        {
            return false;
        }

        if (task.prepare_state != PrepareState::Ready || !task.bitmap)
        {
            return false;
        }

        TextureHandle placeholder = task.texture.handle;
        task.texture.handle = 0;

        task.texture.handle = m_renderer.createTexture(
            task.texture.width, task.texture.height, task.texture.format, nullptr);

        task.texture.sample_width = task.texture.width;
        task.texture.sample_height = task.texture.height;
        task.gpu_texture_ready = true;

        if (placeholder)
        {
            std::lock_guard lock(m_gpu_destroy_mutex);
            m_gpu_destroy_queue.push_back(placeholder);
        }

        return true;
    }

    void TextureCache::setPrefetchDirection(int direction)
    {
        m_prefetch_direction = direction;
    }

    void TextureCache::cancelStaleDecodes(size_t priority_index)
    {
        const size_t count = m_indexer.size();
        if (!count)
        {
            return;
        }

        // Keep in-flight decodes bounded to a window around the visible image. Anything
        // further out is work for images the user already scrolled past; cancelling it
        // frees decode-pool/worker capacity so the visible image's decode isn't starved.
        const size_t keep = texture_prefetch_size;

        std::vector<size_t> stale;

        m_cache.for_each([&] (size_t index, std::shared_ptr<DecodeTask>& task)
        {
            if (index == priority_index || !task)
            {
                return;
            }

            // Only reclaim work still in flight; finished textures stay cached so
            // navigating back to them is instant.
            if (!task->isDecoding())
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

        // Erasing drops the cache's strong reference; the task's deleter routes it to
        // the reaper, which cancels the decoder and joins the future before freeing it.
        for (size_t index : stale)
        {
            if (trace_decode)
            {
                printLine("[trace] #{} cancel-stale (priority #{})", index, priority_index);
            }

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

    void TextureCache::storeTask(size_t index, const std::shared_ptr<DecodeTask>& task)
    {
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
            return;
        }

        // Desired pin window: the current image plus the active prefetch window.
        // Built with the same index math as tickPrefetch() so the pinned set and
        // the prefetched set stay identical.
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

        // Publish the new window before migrating in, so storeTask()/isPinIndex()
        // (and any creation that follows) route window members to the overlay.
        m_pin_set = std::move(desired);

        // Migrate in: window members currently sitting in the evictable cache are
        // promoted to the overlay so eviction can never drop them.
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

    void TextureCache::logDecodeTiming(DecodeTask& task)
    {
        if (task.decode_logged)
        {
            return;
        }

        // The future becomes ready when decode() has fully returned.
        if (!task.future.valid() ||
            task.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }

        task.decode_logged = true;

        const u64 start = task.decode_start_ms.load();
        const u64 first = task.decode_first_ms.load();
        const u64 end = mango::Time::ms();

        if (!start)
        {
            return;
        }

        printLine("[decode] {}: total {} ms, first pixels {} ms ({} x {})",
            task.name,
            end - start,
            first ? (first - start) : 0,
            task.texture.width, task.texture.height);
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
        if (m_shutdown || (m_should_abort && m_should_abort()) ||
            !m_prefetch_direction || texture_prefetch_size == 0)
        {
            return;
        }

        // Don't pile on more decodes while the pipeline is already saturated. This
        // keeps the visible image's decode from being starved by prefetch and bounds
        // the number of full-resolution bitmaps held in memory at once.
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

    void TextureCache::uploadDownscaledPreview(DecodeTask& task)
    {
        GpuTexture& texture = task.texture;

        if (!task.bitmap || !task.scaled_bitmap)
        {
            return;
        }

        const bool needs_create = !task.gpu_texture_ready;
        const u64 now = mango::Time::ms();

        if (!needs_create && now - task.last_preview_ms < 50)
        {
            return;
        }

        task.last_preview_ms = now;

        const int dw = task.downscale_width;
        const int dh = task.downscale_height;

        u32_bicubic_blit(*task.scaled_bitmap, *task.bitmap,
            0.5f, 0.5f, float(dw) - 1.0f, float(dh) - 1.0f);

        if (needs_create)
        {
            TextureHandle placeholder = texture.handle;
            texture.handle = m_renderer.createTexture(dw, dh, texture.format, task.scaled_bitmap->image);
            task.gpu_texture_ready = true;

            if (placeholder)
            {
                std::lock_guard lock(m_gpu_destroy_mutex);
                m_gpu_destroy_queue.push_back(placeholder);
            }

            return;
        }

        m_renderer.uploadTextureRegion(texture.handle, texture.format,
            0, 0, dw, dh, task.scaled_bitmap->image);
    }

    size_t TextureCache::setCurrentPath(const std::string& name)
    {
        m_cache.clear();
        m_pinned.clear();
        m_pin_set.clear();

        std::string filename = name;
        Path temp(filename);

        if (temp.isFile(filename))
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

                std::string pathname = getPath(filename);
                m_current_path = std::make_shared<Path>(pathname);
                m_indexer.start(pathname);
                filename = removePath(filename);
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
        // Establish it before lookup/creation so the visible image is routed to
        // the eviction-immune overlay and protected from this point on.
        if (priority)
        {
            repin(index);
        }

        auto entry = lookupTask(index);
        if (entry)
        {
            return entry;
        }

        std::shared_ptr<DecodeTask> task = makeTask();
        std::string filename = m_indexer[index];

        task->name = filename;
        task->index = index;
        task->file = std::make_unique<File>(*m_current_path, filename);
        task->decoder = std::make_unique<ImageDecoder>(*task->file, filename);
        ImageHeader header = task->decoder->header();

        //printLine("{}: {} x {}", filename, header.width, header.height);
        if (!header.width || !header.height)
        {
            return {};
        }

        const int max_texture_dimension = m_renderer.getMaxTextureDimension();
        const bool needs_downscale = max_texture_dimension > 0 &&
            (header.width > max_texture_dimension || header.height > max_texture_dimension);

        if (needs_downscale && header.format.isFloat())
        {
            printLine(Print::Error, "{}: {} x {} exceeds GPU texture limit (max dimension: {}); float preview not supported yet",
                filename, header.width, header.height, max_texture_dimension);

            createPlaceholderTexture(*task, m_renderer, header.width, header.height);
            task->prepare_state = PrepareState::Failed;

            storeTask(index, task);
            return task;
        }

        GpuTexture& texture = task->texture;
        task->bitmap_format = selectBitmapFormat(header);
        texture.format = selectPixelFormat(header, texture.linear);

        if (needs_downscale)
        {
            const math::int32x2 preview_size = computeDownscaleDimensions(
                header.width, header.height, max_texture_dimension);

            task->downscale = true;
            task->downscale_width = preview_size.x;
            task->downscale_height = preview_size.y;

            printLine(Print::Info, "{}: {} x {} exceeds GPU limit ({}), preview {} x {}",
                filename, header.width, header.height, max_texture_dimension,
                task->downscale_width, task->downscale_height);

            texture.sample_width = task->downscale_width;
            texture.sample_height = task->downscale_height;
        }
        else
        {
            texture.sample_width = header.width;
            texture.sample_height = header.height;
        }

        texture.width = header.width;
        texture.height = header.height;
        task->updates.clear();
        task->progress = 0.0f;

        createPlaceholderTexture(*task, m_renderer, header.width, header.height);

        task->prepare_state = PrepareState::Preparing;

        if (trace_decode)
        {
            printLine("[trace] #{} request (priority={}) {} x {}",
                index, priority ? 1 : 0, header.width, header.height);
        }

        WorkerJob job;
        job.type = WorkerJob::Type::Prepare;
        job.task = task;
        enqueuePrepare(std::move(job), priority);

        storeTask(index, task);
        return task;
    }

    bool TextureCache::updateDecodeTask(DecodeTask& task)
    {
        finishGpuSetup(task);

        GpuTexture& texture = task.texture;

        if (task.prepare_state != PrepareState::Ready || !task.bitmap)
        {
            return false;
        }

        const std::vector<ImageDecodeRect> updates = task.getUpdates();

        if (task.downscale)
        {
            if (!updates.empty() || !task.gpu_texture_ready)
            {
                uploadDownscaledPreview(task);
                return true;
            }

            return false;
        }

        if (!texture.handle || updates.empty())
        {
            return false;
        }

        std::vector<TextureRegionUpload> regions;
        regions.reserve(updates.size());
        std::vector<std::unique_ptr<Bitmap>> temps;

        for (const ImageDecodeRect& rect : updates)
        {
            TextureRegionUpload region =
            {
                .x = rect.x,
                .y = rect.y,
                .width = rect.width,
                .height = rect.height,
            };

            if (task.bitmap->width == rect.width)
            {
                region.pixels = task.bitmap->address(rect.x, rect.y);
            }
            else
            {
                temps.push_back(std::make_unique<Bitmap>(Surface(*task.bitmap, rect.x, rect.y, rect.width, rect.height)));
                region.pixels = temps.back()->image;
            }

            regions.push_back(region);
        }

        const size_t submitted = m_renderer.uploadTextureRegions(texture.handle, texture.format,
            regions.data(), regions.size());

        bool all_uploaded = false;

        {
            std::lock_guard lock(task.mutex);
            for (size_t i = submitted; i < updates.size(); ++i)
            {
                task.updates.push_back(updates[i]);
            }
            all_uploaded = task.updates.empty();
        }

        // Once the decode has fully finished and every region has been uploaded to the
        // GPU, the full-resolution CPU bitmap is dead weight (no re-uploads happen).
        // Releasing it here keeps host RAM bounded while browsing many huge images;
        // the memcpy into the staging buffer already happened during the upload above.
        const bool decode_finished = !task.future.valid() ||
            task.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;

        if (all_uploaded && task.gpu_texture_ready && decode_finished)
        {
            task.bitmap.reset();

            // The image is fully on the GPU; the upload staging buffers are no longer
            // needed, so reclaim them too (the CPU bitmap above was the larger cost,
            // this trims the rest).
            m_renderer.releaseUploadStaging(texture.handle);
        }

        return submitted > 0;
    }

    bool TextureCache::update(size_t priority_index)
    {
        if (m_shutdown || (m_should_abort && m_should_abort()))
        {
            return false;
        }

        bool progress = false;

        drainGpuDestroys(2);

        // Refresh the pin window first so the visible image and its prefetch window
        // are in the eviction-immune overlay before cancelStaleDecodes()/tickPrefetch()
        // run (a prefetch insert here could otherwise evict the just-navigated image).
        repin(priority_index);

        cancelStaleDecodes(priority_index);
        tickPrefetch(priority_index);

        // Adapt the GPU upload budget: stay conservative for a few frames after the
        // visible image changes (so navigation stays snappy), then ramp up so a
        // dwelt-on image sharpens quickly.
        if (priority_index != m_last_priority_index)
        {
            m_last_priority_index = priority_index;
            m_upload_settle_frames = texture_upload_settle_frames;
        }

        if (m_upload_settle_frames > 0)
        {
            --m_upload_settle_frames;
            m_renderer.setUploadBytesPerFrame(texture_upload_bytes_navigating);
        }
        else
        {
            m_renderer.setUploadBytesPerFrame(texture_upload_bytes_idle);
        }

        static constexpr int kBackgroundUploadBudget = 1;

        forEachTask([this] (size_t /*index*/, std::shared_ptr<DecodeTask>& task_ptr)
        {
            logDecodeTiming(*task_ptr);
        });

        // Priority image: always set up and uploaded immediately so the visible image
        // keeps progressing without waiting on a budget.
        auto priority_entry = lookupTask(priority_index);

        if (trace_decode)
        {
            // Report transitions only (this runs at the frame poll rate). The key signal
            // is whether the visible image is even in the cache for update() to service,
            // plus its servicing state when it changes.
            int state = -1; // -1 = missing from cache
            if (priority_entry)
            {
                const DecodeTask& t = *priority_entry;
                state = (t.prepare_state == PrepareState::Ready ? 1 : 0)
                      | (t.gpu_texture_ready ? 2 : 0)
                      | (t.hasPendingUpdates() ? 4 : 0)
                      | (t.isDecoding() ? 8 : 0);
            }

            if (priority_index != m_trace_last_index || state != m_trace_last_state)
            {
                m_trace_last_index = priority_index;
                m_trace_last_state = state;

                if (state < 0)
                {
                    printLine("[trace] #{} PRIORITY MISSING FROM CACHE", priority_index);
                }
                else
                {
                    printLine("[trace] #{} service ready={} gpu={} pending={} decoding={}",
                        priority_index, (state & 1) ? 1 : 0, (state & 2) ? 1 : 0,
                        (state & 4) ? 1 : 0, (state & 8) ? 1 : 0);
                }
            }
        }

        if (priority_entry)
        {
            if (updateDecodeTask(*priority_entry))
            {
                progress = true;
            }
        }

        // Setup pass: create the GPU texture for every ready-but-uncreated prefetched
        // image eagerly. With VMA this is a cheap pool sub-allocation plus an async
        // clear (no fence wait), not the per-image vkAllocateMemory it used to be, and
        // it is naturally rate-limited by the decode pipeline (texture_inflight_decode_limit).
        // Front-loading it means a freshly-prefetched image is displayable (placeholder)
        // the instant it is navigated to, instead of waiting for a budgeted frame.
        // finishGpuSetup() no-ops on already-created, not-yet-ready, and downscale tasks.
        forEachTask([this, priority_index] (size_t index, std::shared_ptr<DecodeTask>& task_ptr)
        {
            if (index == priority_index)
            {
                return;
            }

            finishGpuSetup(*task_ptr);
        });

        // Upload pass: pixel uploads stay throttled. The renderer's byte budget
        // (setUploadBytesPerFrame) is applied per batch, so limiting background work to
        // one batch per frame keeps it a true per-frame cap and protects navigation
        // snappiness. A batch that makes progress sustains the redraw loop for the next.
        int upload_budget = kBackgroundUploadBudget;

        forEachTask([this, priority_index, &upload_budget, &progress]
            (size_t index, std::shared_ptr<DecodeTask>& task_ptr)
        {
            if (index == priority_index)
            {
                return;
            }

            if (upload_budget <= 0)
            {
                return;
            }

            if (!task_ptr->hasPendingUpdates() && task_ptr->gpu_texture_ready)
            {
                return;
            }

            if (updateDecodeTask(*task_ptr))
            {
                progress = true;
                --upload_budget;
            }
        });

        return progress;
    }

} // namespace ifap
