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
#include <mutex>
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

        explicit DecodeTask(VKRenderer& renderer);
        ~DecodeTask();

        bool hasPendingUpdates() const;
        std::vector<ImageDecodeRect> getUpdates();
    };

    class TextureCache
    {
    protected:
        VKRenderer& m_renderer;

        ARCCache<size_t, std::shared_ptr<DecodeTask>> m_cache { texture_cache_size };
        ImageFileIndexer m_indexer;

        std::shared_ptr<Path> m_current_path;

        std::thread m_worker;
        std::mutex m_worker_mutex;
        std::condition_variable m_worker_cv;
        bool m_worker_running = true;

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

        std::deque<WorkerJob> m_worker_jobs;

        std::mutex m_gpu_destroy_mutex;
        std::deque<TextureHandle> m_gpu_destroy_queue;

        int m_prefetch_direction = 0;

        std::shared_ptr<DecodeTask> makeTask();
        void deferDispose(DecodeTask* task);
        void enqueueWorker(WorkerJob job);
        void workerThreadMain();
        void runPrepare(const std::shared_ptr<DecodeTask>& task);
        void runDispose(WorkerJob job);
        void drainGpuDestroys(int budget);
        bool finishGpuSetup(DecodeTask& task);
        void tickPrefetch(size_t priority_index);

    public:
        explicit TextureCache(VKRenderer& renderer);
        ~TextureCache();

        operator const ImageFileIndexer& () const;

        size_t setCurrentPath(const std::string& name);
        std::shared_ptr<DecodeTask> getTexture(size_t index);
        void setPrefetchDirection(int direction);
        bool updateDecodeTask(DecodeTask& task);
        void update(size_t priority_index);

    protected:
        void uploadDownscaledPreview(DecodeTask& task);
    };

} // namespace ifap
