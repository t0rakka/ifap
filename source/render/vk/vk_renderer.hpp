/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "../render_backend.hpp"

#include <memory>
#include <vulkan/vulkan.h>

namespace mango::vulkan
{
    class Instance;
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
        VKRenderer(mango::vulkan::VulkanWindow& window, mango::vulkan::Instance& instance, VkSurfaceKHR surface);
        ~VKRenderer();

        void initialize();
        void resize(int width, int height);

        bool beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend);
        void drawImage(const ImageDrawRequest& request);
        void endFrame();

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
    };

} // namespace ifap
