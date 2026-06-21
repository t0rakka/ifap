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
        float intensity = 1.0f;

        TextureFilter filter = TextureFilter::BILINEAR;
    };

    class RenderBackend
    {
    public:
        virtual ~RenderBackend() = default;

        virtual void initialize() = 0;
        virtual void resize(int width, int height) = 0;

        virtual void beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend) = 0;
        virtual void drawImage(const ImageDrawRequest& request) = 0;

        virtual TextureHandle createTexture(int width, int height, PixelFormat format, const void* initial_data) = 0;
        virtual void uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                         int x, int y, int width, int height, const void* pixels) = 0;
        virtual void destroyTexture(TextureHandle handle) = 0;
    };

} // namespace ifap
