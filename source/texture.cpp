/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "texture.hpp"

#include <mango/image/bicubic.hpp>

#include <algorithm>
#include <memory>

namespace ifap
{

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
            texture.format = PixelFormat::RGBA8_UNORM;
            texture.linear = true;
            texture.handle = renderer.createTexture(1, 1, PixelFormat::RGBA8_UNORM, placeholder_pixel);
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
    }

    TextureCache::~TextureCache()
    {
        shutdown();

        m_cache.clear();

        {
            std::lock_guard lock(m_worker_mutex);
            m_worker_running = false;
        }

        m_worker_cv.notify_all();
        m_worker.join();

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

        m_cache.for_each([] (size_t /*index*/, std::shared_ptr<DecodeTask>& task)
        {
            if (task && task->decoder)
            {
                task->decoder->cancel();
            }
        });

        std::deque<TextureHandle> pending_destroy;

        {
            std::lock_guard lock(m_worker_mutex);

            while (!m_worker_jobs.empty())
            {
                WorkerJob job = std::move(m_worker_jobs.front());
                m_worker_jobs.pop_front();

                if (job.type == WorkerJob::Type::Prepare && job.task && job.task->decoder)
                {
                    job.task->decoder->cancel();
                }
                else if (job.type == WorkerJob::Type::Dispose && job.gpu_handle)
                {
                    pending_destroy.push_back(job.gpu_handle);
                }
            }
        }

        if (!pending_destroy.empty())
        {
            std::lock_guard lock(m_gpu_destroy_mutex);
            for (TextureHandle handle : pending_destroy)
            {
                m_gpu_destroy_queue.push_back(handle);
            }
        }

        m_worker_cv.notify_all();
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
        enqueueWorker(std::move(job));
    }

    void TextureCache::enqueueWorker(WorkerJob job)
    {
        {
            std::lock_guard lock(m_worker_mutex);
            m_worker_jobs.push_back(std::move(job));
        }

        m_worker_cv.notify_one();
    }

    void TextureCache::workerThreadMain()
    {
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
                if (job.type == WorkerJob::Type::Dispose)
                {
                    runDispose(std::move(job));
                }

                continue;
            }

            switch (job.type)
            {
                case WorkerJob::Type::Prepare:
                    runPrepare(job.task);
                    break;

                case WorkerJob::Type::Dispose:
                    runDispose(std::move(job));
                    break;
            }
        }
    }

    void TextureCache::runPrepare(const std::shared_ptr<DecodeTask>& task)
    {
        if (!task || !task->decoder || m_shutdown || (m_should_abort && m_should_abort()))
        {
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

            task->future = task->decoder->launch([this, task = task.get()] (const ImageDecodeRect& rect)
            {
                if (m_shutdown || (m_should_abort && m_should_abort()))
                {
                    return;
                }

                {
                    std::lock_guard lock(task->mutex);
                    task->updates.push_back(rect);
                    task->progress += rect.progress;
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

            m_renderer.destroyTexture(handle);
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

    void TextureCache::tickPrefetch(size_t priority_index)
    {
        if (m_shutdown || (m_should_abort && m_should_abort()) ||
            !m_prefetch_direction || texture_prefetch_size == 0)
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

            if (m_cache.get(index))
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

    std::shared_ptr<DecodeTask> TextureCache::getTexture(size_t index)
    {
        auto entry = m_cache.get(index);
        if (entry)
        {
            return entry.value();
        }

        std::shared_ptr<DecodeTask> task = makeTask();
        std::string filename = m_indexer[index];

        task->file = std::make_unique<File>(*m_current_path, filename);
        task->decoder = std::make_unique<ImageDecoder>(*task->file, filename);
        ImageHeader header = task->decoder->header();

        printLine("{}: {} x {}", filename, header.width, header.height);
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

            m_cache.insert(index, task);
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

        WorkerJob job;
        job.type = WorkerJob::Type::Prepare;
        job.task = task;
        enqueueWorker(std::move(job));

        m_cache.insert(index, task);
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

        if (submitted < updates.size())
        {
            std::lock_guard lock(task.mutex);
            for (size_t i = submitted; i < updates.size(); ++i)
            {
                task.updates.push_back(updates[i]);
            }
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
        tickPrefetch(priority_index);

        static constexpr int kBackgroundUploadBudget = 1;

        m_cache.for_each([this, &progress] (size_t /*index*/, std::shared_ptr<DecodeTask>& task_ptr)
        {
            if (finishGpuSetup(*task_ptr))
            {
                progress = true;
            }
        });

        if (auto entry = m_cache.get(priority_index))
        {
            if (updateDecodeTask(*entry.value()))
            {
                progress = true;
            }
        }

        int budget = kBackgroundUploadBudget;

        m_cache.for_each([this, priority_index, &budget, &progress] (size_t index, std::shared_ptr<DecodeTask>& task_ptr)
        {
            if (index == priority_index || budget <= 0)
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
                --budget;
            }
        });

        return progress;
    }

} // namespace ifap
