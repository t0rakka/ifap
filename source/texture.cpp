/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "texture.hpp"

#include <mango/image/bicubic.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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

        // The three channel layouts ifap ever decodes into. 8-bit for SDR fast paths,
        // fp16 for HDR/float and for the bake target (range + precision through the
        // primaries matrix), fp32 only for >fp16 float sources.
        inline Format formatU8()  { return Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8); }
        inline Format formatU16() { return Format(64, Format::UNORM, Format::RGBA, 16, 16, 16, 16); }
        inline Format formatF16() { return Format(64, Format::FLOAT16, Format::RGBA, 16, 16, 16, 16); }
        inline Format formatF32() { return Format(128, Format::FLOAT32, Format::RGBA, 32, 32, 32, 32); }

        inline Format decodeBitmapFormat(const ImageHeader& header)
        {
            // Palette/indexed sources decode into RGBA; mango resolves the palette on
            // decode when the destination format does not match the file format.
            if (header.format.isIndexed())
            {
                return header.format.bits > 8 ? formatU16() : formatU8();
            }

            if (header.format.isFloat())
            {
                return header.format.bits > 64 ? formatF32() : formatF16();
            }

            if (header.format.bits > 32)
            {
                return formatU16();
            }

            return formatU8();
        }

        // Scene-linear working-space target for linearize(); always fp16 so the final
        // blit never clamps HDR to UNORM/sRGB. Decode uses bitmap_format (RGBA for indexed).
        inline Format formatLinearDest() { return formatF16(); }

        // ---- classification ----------------------------------------------------------

        struct ColorPlan
        {
            PixelFormat upload_format = PixelFormat::RGBA8_SRGB; // GPU texture format
            Format      bitmap_format = formatU8();              // CPU decode target layout
            bool        convert = false;                         // linearize() to scene-linear BT.709
        };

        // Decides how an image reaches the sampler as scene-linear BT.709:
        //   - fast GPU paths use the VkFormat alone (RGBA8_SRGB hw-decode, or linear
        //     UNORM/float passthrough) for BT.709 sRGB/linear content;
        //   - anything else (non-BT.709 primaries, exact non-BT.709 chromaticities, a
        //     transfer that isn't sRGB/linear, or an explicit gamma) is flagged for
        //     mango::linearize() which produces scene-linear BT.709 fp16 per tile;
        //   - ICC-tagged images stay on the hardware sRGB path (linearize() does not
        //     handle ICC; ColorManager is a separate path when we add it).
        ColorPlan classifyColor(const ImageHeader& header, ConstMemory icc)
        {
            const ColorInfo& color = header.color;
            const bool is_float = header.format.isFloat();
            const bool has_icc = icc.size > 0;

            ColorPlan plan;

            // Resolve effective primaries; exact chromaticities take precedence.
            ColorPrimaries prim = color.primaries;
            if (color.has_chromaticities)
            {
                const ColorPrimaries id = identifyPrimaries(color.white, color.red,
                                                            color.green, color.blue);
                prim = id; // Unspecified when no known set matches -> custom -> bake
            }

            TransferFunction transfer = color.transfer;
            if (transfer == TransferFunction::Unspecified)
            {
                transfer = is_float ? TransferFunction::Linear : TransferFunction::sRGB;
            }

            const bool explicit_gamma = color.gamma != 0.0f;
            const bool bt709 = prim == ColorPrimaries::BT709;

            // Fast GPU paths: BT.709 primaries, no ICC, no explicit gamma, transfer the
            // VkFormat can express (sRGB hardware decode or linear passthrough).
            if (bt709 && !has_icc && !explicit_gamma)
            {
                if (transfer == TransferFunction::Linear)
                {
                    if (is_float)
                    {
                        const bool wide = header.format.bits > 64;
                        plan.upload_format = wide ? PixelFormat::RGBA32F : PixelFormat::RGBA16F;
                        plan.bitmap_format = wide ? formatF32() : formatF16();
                    }
                    else if (header.format.bits > 32)
                    {
                        plan.convert = true;
                        plan.upload_format = PixelFormat::RGBA16F;
                        plan.bitmap_format = formatU16();
                    }
                    else
                    {
                        plan.upload_format = PixelFormat::RGBA8_UNORM;
                        plan.bitmap_format = decodeBitmapFormat(header);
                    }
                    return plan;
                }

                if (transfer == TransferFunction::sRGB && !is_float)
                {
                    if (header.format.bits > 32)
                    {
                        plan.convert = true;
                        plan.upload_format = PixelFormat::RGBA16F;
                        plan.bitmap_format = formatU16();
                    }
                    else
                    {
                        plan.upload_format = PixelFormat::RGBA8_SRGB;
                        plan.bitmap_format = decodeBitmapFormat(header);
                    }
                    return plan;
                }
            }

            // ICC: keep on the hardware sRGB path until a ColorManager bake lands. This
            // matches prior behaviour (no regression) rather than guessing a matrix.
            if (has_icc)
            {
                if (is_float)
                {
                    plan.upload_format = PixelFormat::RGBA16F;
                    plan.bitmap_format = formatF16();
                }
                else if (header.format.bits > 32)
                {
                    // ICC + 16-bit PSD/TIFF: decode natively, bake to scene-linear fp16.
                    plan.convert = true;
                    plan.upload_format = PixelFormat::RGBA16F;
                    plan.bitmap_format = formatU16();
                }
                else
                {
                    plan.upload_format = PixelFormat::RGBA8_SRGB;
                    plan.bitmap_format = decodeBitmapFormat(header);
                }
                return plan;
            }

            // Bake path: decode in the source encoding, then linearize() each tile into a
            // dedicated fp16 bitmap (never reuse the decode format as the dest — linearize
            // accepts u8/sRGB surfaces but clamps HDR when used as dest).
            plan.convert = true;
            plan.upload_format = PixelFormat::RGBA16F;
            plan.bitmap_format = decodeBitmapFormat(header);
            return plan;
        }

        // Copy the worker-produced header_* fields into the drawable texture struct.
        // Runs on the UI thread, exactly once, only after prepare_state == Ready has been
        // observed (acquire), so it safely picks up the worker's release-ordered writes.
        // Until this runs the task keeps its neutral 1x1 placeholder dims, so the per-frame
        // draw path only ever reads texture.* written on this same (UI) thread.
        void promoteHeaderDims(DecodeTask& task)
        {
            if (task.header_applied)
            {
                return;
            }

            GpuTexture& texture = task.texture;
            texture.width = task.header_width;
            texture.height = task.header_height;
            texture.sample_width = task.header_sample_width;
            texture.sample_height = task.header_sample_height;
            texture.format = task.header_format;
            texture.linear = task.header_linear;
            task.header_applied = true;
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
        // Create the single shared placeholder up front (before the worker threads start),
        // so every getTexture() can hand it out without allocating a per-image texture.
        static const u8 placeholder_pixel[] = { 32, 32, 32, 255 };
        m_placeholder = m_renderer.createTexture(1, 1, PixelFormat::RGBA8_UNORM, placeholder_pixel);

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
        // Never destroy the shared placeholder; only a per-task texture is owned here.
        TextureHandle handle = raw_task->texture.handle;
        job.gpu_handle = (handle != m_placeholder) ? handle : 0;
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
        if (!task || m_shutdown || (m_should_abort && m_should_abort()))
        {
            return;
        }

        // Evicted before we reached it: the user scrolled past while this prepare sat
        // in the queue, so only this job still references the task. Skip the expensive
        // I/O + bitmap allocation + decode launch instead of doing work nobody will use.
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
            // Bulk, sequential read of the whole compressed file into RAM, on this worker
            // thread (never the UI thread). Going through File(path, name) preserves the
            // custom-mapper / archive support setCurrentPath() relies on; copying its
            // memory into a Buffer forces one sequential read and lets us drop the mapping
            // immediately, so the decode below never has to touch the disk again. The
            // single worker serialises these reads (one file at a time, spinning-disk
            // friendly) while the launched decodes run in parallel on the decode pool.
            //
            // The copy runs in blocks with an abort/abandonment check between them: if the
            // user scrolled past this image while a large read was in progress, we bail
            // here instead of holding the worker until the whole file is read.
            {
                File file(*task->path, task->name);
                ConstMemory src = file;

                auto buffer = std::make_unique<Buffer>(src.size);
                u8* dst = buffer->data();
                bool aborted = false;

                for (size_t offset = 0; offset < src.size; offset += texture_read_block_size)
                {
                    if (m_shutdown || (m_should_abort && m_should_abort()) || task.use_count() <= 1)
                    {
                        aborted = true;
                        break;
                    }

                    const size_t block = std::min(texture_read_block_size, src.size - offset);
                    std::memcpy(dst + offset, src.address + offset, block);
                }

                if (aborted)
                {
                    if (trace_decode)
                    {
                        printLine("[trace] #{} abort-read", task->index);
                    }
                    return;
                }

                task->buffer = std::move(buffer);
            }

            task->decoder = std::make_unique<ImageDecoder>(*task->buffer, *task->path, task->name);
            ImageHeader header = task->decoder->header();

            if (!header.width || !header.height)
            {
                task->prepare_state = PrepareState::Failed;
                if (m_on_content_changed)
                {
                    m_on_content_changed();
                }
                return;
            }

            // Classify the file's colour signalling into a decode/upload plan: a fast GPU
            // format for BT.709 sRGB/linear content, or a CPU bake to scene-linear BT.709
            // fp16 for everything else (non-BT.709 primaries, odd transfer, explicit gamma).
            const ColorPlan plan = classifyColor(header, task->decoder->icc());

            const int max_texture_dimension = m_renderer.getMaxTextureDimension();
            const bool needs_downscale = max_texture_dimension > 0 &&
                (header.width > max_texture_dimension || header.height > max_texture_dimension);

            // The downscale preview path operates on 8-bit bitmaps; float (HDR) and baked
            // (scene-linear fp16) sources have no preview yet.
            if (needs_downscale && (header.format.isFloat() || plan.convert))
            {
                printLine(Print::Error, "{}: {} x {} exceeds GPU texture limit (max dimension: {}); float preview not supported yet",
                    task->name, header.width, header.height, max_texture_dimension);

                task->prepare_state = PrepareState::Failed;
                if (m_on_content_changed)
                {
                    m_on_content_changed();
                }
                return;
            }

            // Header-derived results go into the task's header_* staging fields, never
            // texture.* — those are promoted on the UI thread (promoteHeaderDims) so the
            // per-frame draw path never reads dims/format while we write them here.
            task->bitmap_format = plan.bitmap_format;
            task->header_format = plan.upload_format;
            task->header_linear = true; // sampled texels are scene-linear in every path
            task->needs_color_convert = plan.convert;
            task->header_color = header.color;
            task->header_width = header.width;
            task->header_height = header.height;

            if (needs_downscale)
            {
                const math::int32x2 preview_size = computeDownscaleDimensions(
                    header.width, header.height, max_texture_dimension);

                task->downscale = true;
                task->downscale_width = preview_size.x;
                task->downscale_height = preview_size.y;

                printLine(Print::Info, "{}: {} x {} exceeds GPU limit ({}), preview {} x {}",
                    task->name, header.width, header.height, max_texture_dimension,
                    task->downscale_width, task->downscale_height);

                task->header_sample_width = task->downscale_width;
                task->header_sample_height = task->downscale_height;

                task->scaled_bitmap = std::make_unique<Bitmap>(
                    task->downscale_width, task->downscale_height, task->bitmap_format);
            }
            else
            {
                task->header_sample_width = header.width;
                task->header_sample_height = header.height;
            }

            task->bitmap = std::make_unique<Bitmap>(
                header.width, header.height, task->bitmap_format);

            // Bake path: scene-linear fp16 result for the GPU; decode layout stays in
            // bitmap_format (u8 encoded integer, or fp16/fp32 float).
            if (task->needs_color_convert)
            {
                task->convert_bitmap = std::make_unique<Bitmap>(
                    header.width, header.height, formatLinearDest());
            }

            task->decode_start_ms.store(mango::Time::ms());

            if (trace_decode)
            {
                printLine("[trace] #{} launch {} x {}", task->index, header.width, header.height);
            }

            task->future = task->decoder->launch([this, task = task.get()] (const ImageDecodeRect& rect)
            {
                if (m_shutdown || (m_should_abort && m_should_abort()))
                {
                    return;
                }

                // linearize() this tile to scene-linear BT.709 before publishing it, so the
                // UI thread only ever uploads converted pixels. Dest must be float (fp16)
                // — integer/sRGB dest surfaces clamp HDR during the final blit.
                if (task->needs_color_convert && task->bitmap && task->convert_bitmap)
                {
                    const Surface src(*task->bitmap, rect.x, rect.y, rect.width, rect.height);
                    const Surface dst(*task->convert_bitmap, rect.x, rect.y, rect.width, rect.height);
                    if (dst.format.isFloat())
                    {
                        linearize(dst, src, task->header_color);
                    }
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
            raw->convert_bitmap.reset();
            raw->scaled_bitmap.reset();
            raw->decoder.reset();
            raw->buffer.reset();
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
        // Attempt each currently-queued handle at most once per call. tryDestroyTexture
        // requeues (to the back) any texture an in-flight frame still references, so
        // capping at the initial backlog size both clears the backlog promptly and avoids
        // spinning on busy textures. A small budget caps the work; budget < 0 means "all".
        size_t remaining;
        {
            std::lock_guard lock(m_gpu_destroy_mutex);
            remaining = m_gpu_destroy_queue.size();
        }

        if (budget >= 0 && size_t(budget) < remaining)
        {
            remaining = size_t(budget);
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
        if (task.gpu_texture_ready)
        {
            return false;
        }

        // Check Ready (acquire) before reading task.downscale / task.bitmap: those are
        // written by the worker and only safe to read once the release store is visible.
        if (task.prepare_state != PrepareState::Ready || !task.bitmap || task.downscale)
        {
            return false;
        }

        // Promote the worker's real dimensions/format into the texture before creating the
        // GPU image. This must happen here (not only in updateDecodeTask) because the
        // background setup pass in update() calls finishGpuSetup() directly for prefetched,
        // non-priority tasks — without this they'd be created at the 1x1 placeholder size
        // and, with gpu_texture_ready latched, could never be corrected on navigation.
        promoteHeaderDims(task);

        TextureHandle created = m_renderer.createTexture(
            task.texture.width, task.texture.height, task.texture.format, nullptr);

        if (!created)
        {
            // GPU texture creation failed (e.g. VRAM exhaustion). Keep the placeholder and
            // leave gpu_texture_ready false so we retry on a later frame, once cache
            // eviction has freed device memory. The visible image stays a placeholder
            // rather than crashing on an invalid texture.
            return false;
        }

        TextureHandle placeholder = task.texture.handle;
        task.texture.handle = created;

        task.texture.sample_width = task.texture.width;
        task.texture.sample_height = task.texture.height;
        task.gpu_texture_ready = true;

        // The shared placeholder is owned by the cache and reused by other tasks; only
        // queue a per-task texture for destruction.
        if (placeholder && placeholder != m_placeholder)
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

        // The future is only assigned by the worker once prepare_state flips to Ready
        // (release); don't read it before then (acquire), or we'd race that assignment.
        if (task.prepare_state != PrepareState::Ready)
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
            task.header_width, task.header_height);
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
            TextureHandle created = m_renderer.createTexture(dw, dh, texture.format, task.scaled_bitmap->image);

            if (!created)
            {
                // VRAM exhaustion: keep the placeholder, retry on a later frame.
                return;
            }

            TextureHandle placeholder = texture.handle;
            texture.handle = created;
            task.gpu_texture_ready = true;

            if (placeholder && placeholder != m_placeholder)
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

        task->name = m_indexer[index];
        task->index = index;

        // Capture the current path so the worker can open the file even if the user
        // changes folder (setCurrentPath reassigns m_current_path) while this is queued.
        task->path = m_current_path;

        // All file I/O and header parsing now happen on the worker thread (runPrepare) so
        // a cold, huge file can never seek-stall the UI. The real dimensions aren't known
        // yet, so point at the shared 1x1 placeholder immediately (no per-task allocation);
        // the correct size/format are promoted once the header arrives (promoteHeaderDims),
        // and the full-resolution texture is created in finishGpuSetup.
        GpuTexture& texture = task->texture;
        texture.handle = m_placeholder;
        texture.width = 1;
        texture.height = 1;
        texture.sample_width = 1;
        texture.sample_height = 1;
        texture.format = PixelFormat::RGBA8_UNORM;

        task->prepare_state = PrepareState::Preparing;

        if (trace_decode)
        {
            printLine("[trace] #{} request (priority={})", index, priority ? 1 : 0);
        }

        WorkerJob job;
        job.type = WorkerJob::Type::Prepare;
        job.task = task;
        enqueuePrepare(std::move(job), priority);

        storeTask(index, task, priority);
        return task;
    }

    bool TextureCache::updateDecodeTask(DecodeTask& task)
    {
        // Promote the worker's header dimensions into the drawable texture before any GPU
        // setup or draw uses them. Gated on Ready so it observes the worker's writes.
        if (task.prepare_state == PrepareState::Ready)
        {
            promoteHeaderDims(task);
        }

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

        // On the bake path the GPU samples the scene-linear BT.709 result, not the raw
        // decode; the decode-thread callback has already converted each published rect.
        Bitmap* source = task.needs_color_convert ? task.convert_bitmap.get() : task.bitmap.get();

        if (!texture.handle || updates.empty() || !source)
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

            if (source->width == rect.width)
            {
                region.pixels = source->address(rect.x, rect.y);
            }
            else
            {
                temps.push_back(std::make_unique<Bitmap>(Surface(*source, rect.x, rect.y, rect.width, rect.height)));
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

        if (submitted > 0)
        {
            task.content_uploaded = true;
            task.present_settle_frames = std::max(task.present_settle_frames, 2);
        }

        if (all_uploaded && task.gpu_texture_ready && decode_finished)
        {
            task.bitmap.reset();
            task.convert_bitmap.reset();

            // The decode is done reading from the file Buffer, so drop the decoder (which
            // references the buffer) and then the buffer itself. This is what bounds the
            // extra "whole compressed file in RAM" cost of bulk reads to in-flight decodes.
            task.decoder.reset();
            task.buffer.reset();

            // The image is fully on the GPU; the upload staging buffers are no longer
            // needed, so reclaim them too (the CPU bitmap above was the larger cost,
            // this trims the rest).
            m_renderer.releaseUploadStaging(texture.handle);
        }

        return submitted > 0;
    }

    bool TextureCache::update(size_t priority_index, const std::shared_ptr<DecodeTask>& priority_task)
    {
        if (m_shutdown || (m_should_abort && m_should_abort()))
        {
            return false;
        }

        bool progress = false;

        // Drain the whole backlog each frame: with the non-blocking, header-off-thread
        // pipeline, evictions and placeholder->real swaps can queue textures faster than a
        // fixed tiny budget could free them, starving the descriptor pool / VRAM. Busy
        // textures are safely requeued by tryDestroyTexture.
        drainGpuDestroys(-1);

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
        // Prefer the AppView's current task so a cold drop cannot miss cache lookup.
        std::shared_ptr<DecodeTask> priority_entry = priority_task ? priority_task : lookupTask(priority_index);

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
