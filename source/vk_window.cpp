/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "window.hpp"
#include "command_line.hpp"
#include "app_view.hpp"
#include "render/vk/vk_renderer.hpp"

#include <mango/core/system.hpp>
#include <mango/vulkan/vulkan.hpp>

namespace ifap
{
    using namespace mango;
    using namespace mango::filesystem;
    using namespace mango::vulkan;

    class VKAppWindow : public VulkanWindow
    {
    protected:
        Instance& m_instance;
        const CommandLine& m_commands;
        std::unique_ptr<VKRenderer> m_renderer;
        std::unique_ptr<AppView> m_app;

    public:
        VKAppWindow(Instance& instance, const CommandLine& commands, const VulkanDeviceConfig& config)
            : VulkanWindow(instance, 1280, 800, 0, &config)
            , m_instance(instance)
            , m_commands(commands)
        {
        }

        ~VKAppWindow() override = default;

        void onDeviceReady() override
        {
            SurfaceFormatSelection selection;
            selection.format = surfaceFormat();
            selection.requested = SurfaceFormatIntent::HDR;
            selection.isHdr = isHDR(selection.format);
            logSurfaceFormats(physicalDevice(), surface(), &selection);

            m_renderer = std::make_unique<VKRenderer>(*this, m_instance);
            m_renderer->initialize();
            m_app = std::make_unique<AppView>(*this, *m_renderer);
            m_app->startup(m_commands);
        }

        void onSwapchainResize(VkExtent2D extent) override
        {
            MANGO_UNREFERENCED(extent);
            // Extent-sized resources are rebuilt in beginFrame() after beginDraw()
            // returns a correctly-sized image (ensureProcessingTarget /
            // ensureContentDescriptors). Avoid a second rebuild here.
        }

        void onClose() override { if (m_app) m_app->onClose(); }
        void onMouseMove(int x, int y) override { if (m_app) m_app->onMouseMove(x, y); }
        void onMouseClick(int x, int y, MouseButton button, int count) override
        {
            if (m_app) m_app->onMouseClick(x, y, button, count);
        }
        void onKeyPress(Keycode code, u32 mask) override { if (m_app) m_app->onKeyPress(code, mask); }
        void onDropFiles(const FileIndex& index) override { if (m_app) m_app->onDropFiles(index); }
        void onResize(int width, int height) override { if (m_app) m_app->onResize(width, height); }
        void onFrame(const FrameInfo& info) override { if (m_app) m_app->onFrame(info); }
    };

    static Instance createVulkanInstance(bool enable_validation)
    {
        InstanceExtensionProperties instanceExtensionProperties;

        std::vector<const char*> enabledLayers;
        if (enable_validation)
        {
            enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
        }

        // Resolves and locks the process window system. Call Window::setWindowSystem()
        // before this if you need to override auto-detection (must be before any window).
        std::vector<const char*> enabledExtensions = requiredSurfaceExtensions();

        if (instanceExtensionProperties.contains(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        {
            enabledExtensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }

        VkApplicationInfo applicationInfo =
        {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "ifap",
            .applicationVersion = 1,
            .pEngineName = "mango",
            .engineVersion = 1,
            .apiVersion = VK_MAKE_VERSION(1, 3, 0),
        };

        return Instance(applicationInfo, enabledLayers, enabledExtensions);
    }

    void runApp(const CommandLine& commands)
    {
        const ParsedCommandLine parsed = parseCommandLine(commands);

        if (parsed.options.info)
        {
            printEnable(Print::Info, true);
        }

        VulkanDeviceConfig deviceConfig;
        deviceConfig.surfaceFormatIntent = SurfaceFormatIntent::HDR;

        Instance instance = createVulkanInstance(parsed.options.validate);
        VKAppWindow window(instance, parsed.commands, deviceConfig);
        window.setTitle("iFap Image Viewer");

        EventLoopConfig config;
        config.mode = FrameMode::OnDemand;
        config.waitForFrame = true;

        window.enterEventLoop(config);
    }

} // namespace ifap
