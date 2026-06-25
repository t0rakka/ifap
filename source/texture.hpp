/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "indexer.hpp"
#include "render/vk/vk_renderer.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ifap
{
    using namespace mango;
    using namespace mango::filesystem;
    using namespace mango::image;

    enum class PrepareState
    {
        Pending,
        Preparing,
        Ready,
        Failed,
    };

    struct DecodeTask
    {
        VKRenderer& renderer;

        std::unique_ptr<File> file;
        std::unique_ptr<ImageDecoder> decoder;
        std::unique_ptr<Bitmap> bitmap;
        std::unique_ptr<Bitmap> scaled_bitmap;
        ImageDecodeFuture future;

        GpuTexture texture;

        Format bitmap_format;
        bool downscale = false;
        int downscale_width = 0;
        int downscale_height = 0;
        bool gpu_texture_ready = false;

        std::atomic<PrepareState> prepare_state { PrepareState::Pending };

        mutable std::mutex mutex;
        std::vector<ImageDecodeRect> updates;
        float progress = 0.0f;
        u64 last_preview_ms = 0;

        // Timing instrumentation.
        std::string name;
        std::atomic<u64> decode_start_ms { 0 };  // set on worker just before launch()
        std::atomic<u64> decode_first_ms { 0 };  // first decode callback (first pixels)
        bool decode_logged = false;              // main thread only

        explicit DecodeTask(VKRenderer& renderer);
        ~DecodeTask();

        bool hasPendingUpdates() const;
        std::vector<ImageDecodeRect> getUpdates();

        // True while the async decode future is launched but not yet finished.
        // Safe to poll from the main thread: a task that is being disposed has
        // already been removed from the cache, so the reaper never races this.
        bool isDecoding() const;
    };

    class TextureCache
    {
    protected:
        VKRenderer& m_renderer;
        std::function<void()> m_on_content_changed;
        std::function<bool()> m_should_abort;

        std::atomic<bool> m_shutdown { false };

        // Navigation tracking for the adaptive upload budget.
        size_t m_last_priority_index = size_t(-1);
        int m_upload_settle_frames = 0;

        ARCCache<size_t, std::shared_ptr<DecodeTask>> m_cache { texture_cache_size };
        ImageFileIndexer m_indexer;

        std::shared_ptr<Path> m_current_path;

        struct WorkerJob
        {
            enum class Type
            {
                Prepare,
                Dispose,
            };

            Type type = Type::Prepare;
            std::shared_ptr<DecodeTask> task;
            TextureHandle gpu_handle = 0;
        };

        // Prepare lane: allocates bitmaps and launches the async decode. These jobs
        // are quick and must never wait, so they live on their own thread.
        std::thread m_worker;
        std::mutex m_worker_mutex;
        std::condition_variable m_worker_cv;
        bool m_worker_running = true;
        std::deque<WorkerJob> m_worker_jobs;

        // Disposal lane: joins the decode future (which can block until the decode
        // finishes/cancels) and frees CPU buffers. Kept on a separate "reaper" thread
        // so a blocking join can never stall navigation or prefetch.
        std::thread m_reaper;
        std::mutex m_reaper_mutex;
        std::condition_variable m_reaper_cv;
        bool m_reaper_running = true;
        std::deque<WorkerJob> m_reaper_jobs;

        std::mutex m_gpu_destroy_mutex;
        std::deque<TextureHandle> m_gpu_destroy_queue;

        int m_prefetch_direction = 0;

        std::shared_ptr<DecodeTask> makeTask();
        void deferDispose(DecodeTask* task);
        void enqueuePrepare(WorkerJob job);
        void enqueueDispose(WorkerJob job);
        void workerThreadMain();
        void reaperThreadMain();
        void runPrepare(const std::shared_ptr<DecodeTask>& task);
        void runDispose(WorkerJob job);
        void drainGpuDestroys(int budget);
        bool finishGpuSetup(DecodeTask& task);
        void logDecodeTiming(DecodeTask& task);
        size_t countActiveDecodes() const;
        void tickPrefetch(size_t priority_index);

    public:
        explicit TextureCache(VKRenderer& renderer,
                              std::function<void()> on_content_changed = {},
                              std::function<bool()> should_abort = {});
        ~TextureCache();

        void shutdown();

        operator const ImageFileIndexer& () const;

        size_t setCurrentPath(const std::string& name);
        std::shared_ptr<DecodeTask> getTexture(size_t index);
        void setPrefetchDirection(int direction);
        bool updateDecodeTask(DecodeTask& task);
        bool update(size_t priority_index);

    protected:
        void uploadDownscaledPreview(DecodeTask& task);
    };

} // namespace ifap
