/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "indexer.hpp"
#include "render/vk/vk_renderer.hpp"

#include <mango/core/buffer.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

        // Captured on the UI thread at request time so the worker has a stable path to
        // open even if the user changes folder (setCurrentPath reassigns the cache's
        // current path) while this prepare is in flight.
        std::shared_ptr<Path> path;

        // The whole compressed file, read once into RAM on the worker thread (bulk
        // sequential I/O, off the UI thread). The decoder reads from this, so it must
        // outlive the async decode; it is released once the decode has finished.
        std::unique_ptr<Buffer> buffer;
        std::unique_ptr<ImageDecoder> decoder;
        std::unique_ptr<Bitmap> bitmap;
        std::unique_ptr<Bitmap> scaled_bitmap;
        ImageDecodeFuture future;

        GpuTexture texture;

        // Header parse results, produced by the worker (runPrepare) before prepare_state
        // flips to Ready, then copied into `texture` exactly once on the UI thread
        // (promoteHeaderDims, gated by prepare_state's release/acquire). Kept separate so
        // the worker never writes the dims/format the per-frame draw path reads.
        int header_width = 0;
        int header_height = 0;
        int header_sample_width = 0;
        int header_sample_height = 0;
        PixelFormat header_format = PixelFormat::RGBA8_UNORM;
        bool header_linear = false;
        bool header_applied = false; // UI-only guard for one-shot promotion

        Format bitmap_format;
        bool downscale = false;
        int downscale_width = 0;
        int downscale_height = 0;
        bool gpu_texture_ready = false;

        // Set once a pixel upload submit succeeds; gates "displayed" so a cleared GPU
        // image alone does not end the cold-drop pump before real content lands.
        bool content_uploaded = false;

        // Main-thread only: keep presenting for a few frames after an upload submit.
        int present_settle_frames = 0;

        // Input -> scene-linear BT.709 color pipeline. When needs_color_convert is set the
        // worker decodes into `bitmap` (native encoded layout) and linearize()s each rect
        // into `convert_bitmap` (always fp16 scene-linear BT.709), which is what gets
        // uploaded. Fast-path images (BT.709 sRGB/linear, handled by the VkFormat) leave
        // this clear and upload straight from `bitmap`. ICC-tagged images also skip
        // linearize() and stay on the hardware sRGB path until ColorManager lands.
        bool needs_color_convert = false;
        ColorInfo header_color;
        std::unique_ptr<Bitmap> convert_bitmap;

        std::atomic<PrepareState> prepare_state { PrepareState::Pending };

        mutable std::mutex mutex;
        std::vector<ImageDecodeRect> updates;
        float progress = 0.0f;
        u64 last_preview_ms = 0;

        // Timing instrumentation.
        std::string name;
        size_t index = 0;                        // position in the indexer (for tracing)
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

        // Decode-trace transition tracking (avoids logging every poll-rate frame).
        size_t m_trace_last_index = size_t(-1);
        int m_trace_last_state = -2;

        // Evictable background store for images outside the active window. The
        // eviction policy here is a swappable detail (ARCCache / LRUCache / ...):
        // the visible image and its prefetch window are NOT kept here, they live
        // in m_pinned and are immune to eviction, so the cache policy can never
        // strand the on-screen image regardless of its retention heuristics.
        ARCCache<size_t, std::shared_ptr<DecodeTask>> m_cache { texture_cache_size };

        // Pinned overlay: the current image plus the prefetch window. Entries here
        // are never evicted. As navigation moves the window, entries that fall out
        // are migrated back into m_cache (still resident, now evictable) and ones
        // that enter are pulled out of m_cache. A task lives in exactly one store.
        std::unordered_map<size_t, std::shared_ptr<DecodeTask>> m_pinned;
        std::vector<size_t> m_pin_set;

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

        // One shared 1x1 placeholder texture, reused by every not-yet-ready image instead
        // of allocating a per-task placeholder. This avoids doubling texture/descriptor
        // creation (placeholder + real) and the destroy-queue churn that exhausts the
        // descriptor pool under fast navigation. Never queued for destruction by tasks;
        // owned by the cache and freed in the destructor.
        TextureHandle m_placeholder = 0;

        int m_prefetch_direction = 0;

        std::shared_ptr<DecodeTask> makeTask();
        void deferDispose(DecodeTask* task);
        void enqueuePrepare(WorkerJob job, bool front = false);
        void enqueueDispose(WorkerJob job);
        void cancelStaleDecodes(size_t priority_index);

        // Pinned-overlay helpers (see m_pinned).
        bool isPinIndex(size_t index) const;
        std::shared_ptr<DecodeTask> lookupTask(size_t index);
        void storeTask(size_t index, const std::shared_ptr<DecodeTask>& task, bool pin_overlay = false);
        void repin(size_t priority_index);
        void forEachTask(const std::function<void(size_t, std::shared_ptr<DecodeTask>&)>& fn);

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
        std::shared_ptr<DecodeTask> getTexture(size_t index, bool priority = false);
        void setPrefetchDirection(int direction);
        bool updateDecodeTask(DecodeTask& task);
        bool update(size_t priority_index, const std::shared_ptr<DecodeTask>& priority_task = {});

    protected:
        void uploadDownscaledPreview(DecodeTask& task);
    };

} // namespace ifap
