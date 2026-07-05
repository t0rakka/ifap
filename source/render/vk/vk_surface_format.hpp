/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include <mango/vulkan/vulkan.hpp>

namespace ifap
{

    // Swapchain format preference for VulkanWindow::VulkanDeviceConfig.
    // HDR-first auto selection; see comments in vk_renderer.cpp for platform notes.
    mango::vulkan::VulkanDeviceConfig makeVulkanDeviceConfig();

    void logSelectedSurfaceFormat(mango::vulkan::VulkanWindow& window);

} // namespace ifap
