/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include <mango/mango.hpp>
#include <mango/math/vector.hpp>
#include <mango/opengl/opengl.hpp>

namespace ifap
{

    using mango::u64;
    using mango::math::float32x2;

    static constexpr size_t texture_cache_size = 16;
    static constexpr size_t texture_prefetch_size = 4;
    static constexpr u64 repeat_treshold = 420;
    static constexpr u64 repeat_delay = 3;

} // namespace ifap
