/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2026 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include <mango/mango.hpp>
#include <mango/math/vector.hpp>

#include <mutex>

namespace ifap
{

    // Serializes mango::filesystem access between the indexer thread and the texture
    // worker (concurrent Path iteration + File::map is not safe on Windows).
    // Recursive so the indexer can descend Path(parent, child) while holding the lock.
    extern std::recursive_mutex filesystem_mutex;
    using mango::u64;
    using mango::math::float32x2;

    // Slot ceilings for small images (hackatron-sized JPEGs). Extreme resolutions
    // shrink the live window from texture_memory_budget_bytes so a folder of
    // 16k² RGBA (~1 GB/frame) cannot pin tens of gigabytes and start paging.
    static constexpr size_t texture_cache_size = 24;
    static constexpr size_t texture_prefetch_size = 12;

    // Soft cap on resident decoded image memory (pinned window + evictable cache +
    // pending GPU destroys). Sized for a couple of 16k² RGBA frames (~1 GB each)
    // plus headroom — above this, macOS unified memory starts paging hard.
    static constexpr size_t texture_memory_budget_bytes = 3ull << 30; // 3 GiB

    // Upper bound on decodes in flight at once (the visible image plus prefetch).
    // Elevated so prepare workers and the GPU queue stay fed while scrolling;
    // each in-flight decode holds its compressed file and, on the CPU path, a
    // full-resolution bitmap. Large frames clamp this via the memory budget.
    static constexpr size_t texture_inflight_decode_limit = 12;

    // The worker reads each file into RAM in blocks of this size, checking for abort /
    // abandonment between blocks. This keeps a stale read (e.g. a huge file the user just
    // scrolled past) from holding the single worker thread until the whole file is read.
    static constexpr size_t texture_read_block_size = 8 * 1024 * 1024;

    static constexpr u64 repeat_treshold = 420;
    static constexpr u64 repeat_delay = 3;

    // While arrow keys auto-repeat (~3 ms), only commit a new visible index this often
    // so the priority decode can surface pixels instead of staying on gray placeholders.
    static constexpr u64 nav_latch_interval_ms = 10;

} // namespace ifap
