/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2026 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "../render_backend.hpp"

#include <memory>
#include <vulkan/vulkan.h>
#include <mango/vulkan/texture_decoder.hpp>

namespace mango::vulkan
{
    class VulkanWindow;
}

namespace ifap
{

    class VKRenderer
    {
    protected:
        struct Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        VKRenderer(mango::vulkan::VulkanWindow& window);
        ~VKRenderer();

        void initialize();
        void resize(int width, int height);

        bool beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend);
        void drawImage(const ImageDrawRequest& request);
        void drawSolidRect(int x, int y, int width, int height, float r, float g, float b, float a);
        void endFrame();

        int swapchainWidth() const;
        int swapchainHeight() const;

        int getMaxTextureDimension() const;

        TextureHandle createTexture(int width, int height, PixelFormat format, const void* initial_data);
        void uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                 int x, int y, int width, int height, const void* pixels);
        // Returns the number of regions submitted (0 when upload slots are busy).
        size_t uploadTextureRegions(TextureHandle handle, PixelFormat format,
                                    const TextureRegionUpload* regions, size_t count);
        void destroyTexture(TextureHandle handle);

        // Non-blocking destroy: if the texture still has GPU uploads in flight it is
        // left intact and false is returned (caller should retry later). Never waits
        // on a fence, so it is safe to call every frame on the main thread.
        bool tryDestroyTexture(TextureHandle handle);

        // Reclaim the persistent upload staging buffers for a texture that is fully
        // uploaded (no more region uploads expected). Frees host memory; idle slots
        // re-allocate lazily if another upload ever arrives.
        void releaseUploadStaging(TextureHandle handle);

        // Sets the per-frame GPU upload budget (bytes copied/transferred per upload
        // submit). The cache lowers this while navigating and raises it when idle.
        void setUploadBytesPerFrame(size_t bytes);

        // True once the GPU has retired the most recent upload/clear for this texture.
        bool isTextureUploadComplete(TextureHandle handle) const;

        // True once an upload/clear submit has retired and the image is sampleable.
        bool isTextureLayoutReady(TextureHandle handle) const;

        // Decoder sharing this renderer's device, graphics queue and allocator. Its
        // process() must run on the thread that renders (it submits to the queue).
        std::unique_ptr<mango::vulkan::TextureDecoder> createTextureDecoder(
            const mango::vulkan::TextureDecoderConfig& config);

        // Takes ownership of a decoded image (TextureStatus::Ready, in
        // SHADER_READ_ONLY_OPTIMAL layout) and returns a regular texture handle,
        // released with destroyTexture()/tryDestroyTexture(). Returns 0 and frees
        // the image when the view cannot be created.
        TextureHandle adoptTexture(const mango::vulkan::ImageAllocation& image,
                                   const mango::vulkan::TextureInfo& info);

        // Frees an image from a failed or cancelled decode.
        void destroyImage(const mango::vulkan::ImageAllocation& image);
    };

} // namespace ifap
