/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"

#include <cstdint>

namespace ifap
{

    enum class TextureFilter
    {
        NEAREST,
        BILINEAR,
        BICUBIC
    };

    enum class PixelFormat
    {
        RGBA8_UNORM,
        RGBA8_SRGB,
        RGBA16F,
        RGBA32F,
    };

    using TextureHandle = uint64_t;

    struct GpuTexture
    {
        TextureHandle handle = 0;
        int width = 0;
        int height = 0;
        int sample_width = 0;
        int sample_height = 0;
        PixelFormat format = PixelFormat::RGBA8_UNORM;
        bool linear = false;

        explicit operator bool () const
        {
            return handle != 0;
        }
    };

    struct ImageDrawRequest
    {
        TextureHandle texture = 0;
        int width = 0;
        int height = 0;
        bool linear = false;

        float32x2 translate;
        float32x2 scale;

        TextureFilter filter = TextureFilter::BILINEAR;
    };

    struct TextureRegionUpload
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        const void* pixels = nullptr;
    };

} // namespace ifap
