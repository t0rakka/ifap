/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "vk_surface_format.hpp"

#include <mango/core/print.hpp>

namespace ifap
{
    using namespace mango;
    using namespace mango::vulkan;

    namespace
    {
        // See vk_renderer.cpp (OutputTransform) for Hyprland/Wayland HDR windowed flicker notes.
        constexpr bool kDevForceSurfaceFormat = false;

        constexpr VkSurfaceFormatKHR kDevSurfaceFormat =
        {
            VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT
        };

        constexpr VkSurfaceFormatKHR kPreferredSurfaceFormats[] =
        {
            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_ST2084_EXT },
            { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },
            { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },

            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_HLG_EXT },
            { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_HLG_EXT },

            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_BT2020_LINEAR_EXT },
            { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_BT2020_LINEAR_EXT },

            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT },
            { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT },
            { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT },

            { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
            { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        };

        VkPhysicalDeviceVulkan12Features s_features12 =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .timelineSemaphore = VK_TRUE,
        };

        VkPhysicalDeviceVulkan13Features s_features13 =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &s_features12,
            .dynamicRendering = VK_TRUE,
        };

    } // namespace

    VulkanDeviceConfig makeVulkanDeviceConfig()
    {
        VulkanDeviceConfig config;
        config.deviceCreateInfoPNext = &s_features13;

        if (kDevForceSurfaceFormat)
        {
            config.preferredFormats = { kDevSurfaceFormat };
        }
        else
        {
            config.preferredFormats.assign(std::begin(kPreferredSurfaceFormats),
                                           std::end(kPreferredSurfaceFormats));
        }

        return config;
    }

    void logSelectedSurfaceFormat(VulkanWindow& window)
    {
        const VkSurfaceFormatKHR selected = window.surfaceFormat();
        const std::vector<VkSurfaceFormatKHR> surfaceFormats =
            getSurfaceFormats(window.physicalDevice(), window.surface());

        printLine(Print::Info, "VKRenderer: PhysicalDeviceSurfaceFormats:");

        for (const VkSurfaceFormatKHR& format : surfaceFormats)
        {
            const bool is_selected = format.format == selected.format
                && format.colorSpace == selected.colorSpace;
            printLine(Print::Info, "  {} {} | {}",
                is_selected ? ">" : " ",
                getString(format.format),
                getString(format.colorSpace));
        }

        if (kDevForceSurfaceFormat)
        {
            printLine(Print::Info, "VKRenderer: using dev-forced surface format");
        }
    }

} // namespace ifap
