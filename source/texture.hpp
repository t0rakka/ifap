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

        // Decoded VkImage footprint once known (TextureReport::image_bytes); until
        // then estimatedBytes() falls back to header/sample dimensions × 4.
        size_t image_bytes = 0;

        // True when this prepare was kicked off as the visible/priority image.
        bool priority = false;

        bool isDecoding() const
        {
            return prepare_state.load() == PrepareState::Preparing;
        }

        size_t estimatedBytes() const
        {
            if (image_bytes)
                return image_bytes;

            const int w = gpu_texture_ready ? texture.sample_width : header_width;
            const int h = gpu_texture_ready ? texture.sample_height : header_height;
            if (w > 0 && h > 0)
                return size_t(w) * size_t(h) * 4;

            return 0;
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

        // Evictable background store for images outside the active window. Budgeted
        // by decoded-image bytes (not slot count) so one 16k frame costs what ~250
        // hackatron JPEGs would. The visible image and its prefetch window live in
        // m_pinned and are immune to eviction.
        CostLRUCache<size_t, std::shared_ptr<DecodeTask>> m_cache { texture_memory_budget_bytes };

        // Pinned overlay: the current image plus the prefetch window. Entries here
        // are never evicted. As navigation moves the window, entries that fall out
        // are migrated back into m_cache (still resident, now evictable) and ones
        // that enter are pulled out of m_cache. A task lives in exactly one store.
        std::unordered_map<size_t, std::shared_ptr<DecodeTask>> m_pinned;
        std::vector<size_t> m_pin_set;

        // Largest decoded/header footprint seen in the current folder. Drives the
        // live prefetch/inflight clamps; reset in setCurrentPath(). Atomic so the
        // file-read worker can bump it from a huge compressed size before header.
        std::atomic<size_t> m_typical_image_bytes { 0 };

        // Last Ready task chosen for painting. Survives budget eviction so hold-
        // to-scroll still has a mid-journey frame when the live index is gray.
        mutable std::shared_ptr<DecodeTask> m_display_holdout;

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
        // once the GPU no longer references them. Bytes are tracked separately because
        // a dropped DecodeTask no longer appears in residentBytes() while its VkImage
        // can sit here for several frames (tryDestroy waits on the timeline) — that
        // hole was letting extreme/ ramp to 60+ GB.
        struct PendingDestroy
        {
            TextureHandle handle = 0;
            size_t bytes = 0;
        };
        std::mutex m_gpu_destroy_mutex;
        std::deque<PendingDestroy> m_gpu_destroy_queue;
        std::atomic<size_t> m_pending_destroy_bytes { 0 };

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
        std::shared_ptr<DecodeTask> peekTask(size_t index) const;
        void storeTask(size_t index, const std::shared_ptr<DecodeTask>& task, bool pin_overlay = false);
        void repin(size_t priority_index);
        void forEachTask(const std::function<void(size_t, std::shared_ptr<DecodeTask>&)>& fn);

        void workerThreadMain();
        void runPrepare(const std::shared_ptr<DecodeTask>& task);
        void onDecoded(const std::weak_ptr<DecodeTask>& weak, const mango::vulkan::TextureResult& result);
        void drainGpuDestroys(bool force = false);
        void queueGpuDestroy(TextureHandle handle, size_t bytes);
        // UI thread only: wait out the timeline and free now (eviction under pressure).
        void reclaimTaskGpu(DecodeTask& task);
        void promoteHeader(DecodeTask& task);
        size_t countActiveDecodes() const;
        void tickPrefetch(size_t priority_index);
        bool isAborting() const;

        // Memory budget: clamp live window to texture_memory_budget_bytes.
        size_t prefetchLimit() const;
        size_t inflightLimit() const;
        size_t taskCost(const DecodeTask& task) const;
        size_t residentBytes() const;
        size_t pendingDestroyBytes() const;
        size_t pressureBytes() const;
        void observeImageBytes(size_t bytes);
        void refreshCacheCost(size_t index, const std::shared_ptr<DecodeTask>& task);
        // keep_index: never drop this pin (visible / just-finished). size_t(-1) = cache only.
        void enforceMemoryBudget(size_t keep_index = size_t(-1));

    public:
        explicit TextureCache(VKRenderer& renderer,
                              std::function<void()> on_content_changed = {},
                              std::function<bool()> should_abort = {});
        ~TextureCache();

        void shutdown();

        operator const ImageFileIndexer& () const;

        size_t setCurrentPath(const std::string& name);
        // priority: pin window + front-of-queue. abort_others: cancel sibling in-flight
        // prepares (tap / settle). Hold-to-scroll passes false so Ready frames keep
        // landing on the trail while the title index races ahead.
        std::shared_ptr<DecodeTask> getTexture(size_t index, bool priority = false, bool abort_others = true);
        void setPrefetchDirection(int direction);
        size_t activeDecodeCount() const { return countActiveDecodes(); }
        size_t maxInflightDecodes() const { return inflightLimit(); }

        // Paint helper: prefer `index` when Ready, else walk the scroll trail
        // (opposite of scroll_direction) for the nearest Ready texture. Lets hold-
        // to-scroll show mid-journey frames while the live index catches up.
        std::shared_ptr<DecodeTask> findDisplayTask(size_t index, int scroll_direction) const;

        bool update(size_t priority_index, const std::shared_ptr<DecodeTask>& priority_task = {});

        // Owner thread: process() until every in-flight decode has fired its
        // callback. Used to keep a priority decode off the vsync present queue.
        void wait();
    };

} // namespace ifap
