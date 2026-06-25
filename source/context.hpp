/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include <mango/mango.hpp>
#include <mango/math/vector.hpp>

namespace ifap
{

    using mango::u64;
    using mango::math::float32x2;

    static constexpr size_t texture_cache_size = 16;
    static constexpr size_t texture_prefetch_size = 4;

    // Upper bound on decodes running at once (the visible image plus prefetch).
    // Bounds peak RAM (each in-flight decode owns a full-resolution bitmap) and
    // keeps the shared decode thread pool from thrashing across huge images.
    static constexpr size_t texture_inflight_decode_limit = 3;

    // Per-frame GPU upload budget. While the user is actively navigating we keep each
    // frame's copy/transfer small so input stays snappy; once they settle on an image
    // we push more bytes per frame so large images sharpen quickly.
    static constexpr size_t texture_upload_bytes_navigating = 8 * 1024 * 1024;
    static constexpr size_t texture_upload_bytes_idle = 32 * 1024 * 1024;
    // Frames to stay in the conservative budget after the visible image changes.
    static constexpr int texture_upload_settle_frames = 8;

    static constexpr u64 repeat_treshold = 420;
    static constexpr u64 repeat_delay = 3;

} // namespace ifap
