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

    class VKRenderer : public RenderBackend
    {
    protected:
        struct Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        VKRenderer(mango::vulkan::VulkanWindow& window, mango::vulkan::Instance& instance, VkSurfaceKHR surface);
        ~VKRenderer() override;

        void initialize() override;
        void resize(int width, int height) override;

        void beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend) override;
        void drawImage(const ImageDrawRequest& request) override;
        void endFrame() override;

        int getMaxTextureDimension() const override;

        TextureHandle createTexture(int width, int height, PixelFormat format, const void* initial_data) override;
        void uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                 int x, int y, int width, int height, const void* pixels) override;
        void destroyTexture(TextureHandle handle) override;
    };

} // namespace ifap
