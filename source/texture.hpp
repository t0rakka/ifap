/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2026 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "indexer.hpp"
#include "render/vk/vk_renderer.hpp"

#include <mango/core/buffer.hpp>
#include <mango/vulkan/texture_decoder.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
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
        Preparing,  // file read or decode in flight
        Ready,      // texture on the GPU
        Failed,
    };

    struct DecodeTask
    {
        // Captured on the UI thread at request time so the worker has a stable path to
        // open even if the user changes folder while this prepare is queued.
        std::shared_ptr<Path> path;
        std::string name;
        size_t index = 0;

        std::atomic<PrepareState> prepare_state { PrepareState::Pending };

        // TextureDecoder handle while the decode is in flight (0 before decode() and
        // after the callback). The task's deleter cancels it.
        std::atomic<u64> decode_handle { 0 };

        // Header dimensions published by the worker right after decode() returns, so
        // the placeholder takes the image's aspect before the pixels arrive. Promoted
        // into `texture` on the UI thread.
        int header_width = 0;
        int header_height = 0;
        std::atomic<bool> header_ready { false };

        // UI thread only.
        GpuTexture texture;
        bool gpu_texture_ready = false;
        int present_settle_frames = 0;

        bool isDecoding() const
        {
            return prepare_state.load() == PrepareState::Preparing;
        }
    };

    class TextureCache
    {
    protected:
        VKRenderer& m_renderer;
        std::function<void()> m_on_content_changed;
        std::function<bool()> m_should_abort;

        std::atomic<bool> m_shutdown { false };

        // Decodes every format into a VkImage: JPEG on Vulkan compute, the rest on
        // the CPU, colour conversion and downscaling included. process() runs from
        // update() on the UI thread, which owns the graphics queue.
        std::unique_ptr<mango::vulkan::TextureDecoder> m_decoder;

        // Callbacks fired since the last update(), so update() can report progress.
        int m_completed = 0;

        // Evictable background store for images outside the active window. The
        // visible image and its prefetch window are NOT kept here, they live in
        // m_pinned and are immune to eviction, so the cache policy can never strand
        // the on-screen image regardless of its retention heuristics.
        ARCCache<size_t, std::shared_ptr<DecodeTask>> m_cache { texture_cache_size };

        // Pinned overlay: the current image plus the prefetch window. Entries here
        // are never evicted. As navigation moves the window, entries that fall out
        // are migrated back into m_cache (still resident, now evictable) and ones
        // that enter are pulled out of m_cache. A task lives in exactly one store.
        std::unordered_map<size_t, std::shared_ptr<DecodeTask>> m_pinned;
        std::vector<size_t> m_pin_set;

        ImageFileIndexer m_indexer;

        std::shared_ptr<Path> m_current_path;

        // File read lane: reads each file into RAM (chunked, abortable) and hands it
        // to the decoder. One file at a time keeps spinning disks sequential.
        std::thread m_worker;
        std::mutex m_worker_mutex;
        std::condition_variable m_worker_cv;
        bool m_worker_running = true;
        std::deque<std::shared_ptr<DecodeTask>> m_worker_jobs;

        // Texture handles released by tasks on any thread, destroyed on the UI thread
        // once the GPU no longer references them.
        std::mutex m_gpu_destroy_mutex;
        std::deque<TextureHandle> m_gpu_destroy_queue;

        // One shared 1x1 placeholder texture, reused by every not-yet-ready image.
        TextureHandle m_placeholder = 0;

        int m_prefetch_direction = 0;

        // Partial file reads preserved across abort/resume. Keyed by indexer index;
        // the worker stashes a prefix when a chunked read is abandoned, and the next
        // prepare for that index picks it up. Bounded so fast scrolling cannot retain
        // an unbounded number of huge half-read buffers.
        struct PartialFileRead
        {
            std::unique_ptr<Buffer> buffer;
            size_t bytes = 0;
        };

        std::mutex m_partial_mutex;
        std::unordered_map<size_t, PartialFileRead> m_partial_reads;

        void stashPartialRead(size_t index, std::unique_ptr<Buffer> buffer, size_t bytes);
        PartialFileRead takePartialRead(size_t index);
        void clearPartialReads();

        std::shared_ptr<DecodeTask> makeTask();
        void releaseTask(DecodeTask* task);
        void enqueuePrepare(std::shared_ptr<DecodeTask> task, bool front);
        void abortNonPriorityWork(size_t priority_index);
        void cancelStaleDecodes(size_t priority_index);

        // Pinned-overlay helpers (see m_pinned).
        bool isPinIndex(size_t index) const;
        std::shared_ptr<DecodeTask> lookupTask(size_t index);
        void storeTask(size_t index, const std::shared_ptr<DecodeTask>& task, bool pin_overlay = false);
        void repin(size_t priority_index);
        void forEachTask(const std::function<void(size_t, std::shared_ptr<DecodeTask>&)>& fn);

        void workerThreadMain();
        void runPrepare(const std::shared_ptr<DecodeTask>& task);
        void onDecoded(const std::weak_ptr<DecodeTask>& weak, const mango::vulkan::TextureResult& result);
        void drainGpuDestroys();
        void promoteHeader(DecodeTask& task);
        size_t countActiveDecodes() const;
        void tickPrefetch(size_t priority_index);
        bool isAborting() const;

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
        bool update(size_t priority_index, const std::shared_ptr<DecodeTask>& priority_task = {});

        // Owner thread: process() until every in-flight decode has fired its
        // callback. Used to keep a priority decode off the vsync present queue.
        void wait();
    };

} // namespace ifap
