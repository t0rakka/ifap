/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "window.hpp"
#include "app_view.hpp"
#include "render/vk/vk_renderer.hpp"

#include <mango/core/system.hpp>
#include <mango/vulkan/vulkan.hpp>

namespace ifap
{
    using namespace mango;
    using namespace mango::filesystem;
    using namespace mango::vulkan;

    namespace
    {
        struct IfapArgs
        {
            bool validate = false;
            bool info = false;
            bool sdr = false;
        };

        void configureParser(CommandLineParser& parser, IfapArgs& args)
        {
            parser.usage("[options] [image-or-folder]");

            parser.flag("--info", "enable decode timing and informational output",
                [&]()
                {
                    args.info = true;
                });

            parser.flag("--validate", "enable Khronos validation layer",
                [&]()
                {
                    args.validate = true;
                });

            parser.flag("--sdr", "force SDR swapchain (sRGB / Rec.709)",
                [&]()
                {
                    args.sdr = true;
                });
        }

    } // namespace

    class VKAppWindow : public VulkanWindow
    {
    protected:
        std::string_view m_initial_path;
        SurfaceFormatIntent m_requestedFormat = SurfaceFormatIntent::HDR;
        std::unique_ptr<VKRenderer> m_renderer;
        std::unique_ptr<AppView> m_app;

    public:
        VKAppWindow(VulkanContext& context, std::string_view initial_path, const VulkanDeviceConfig& config)
            : VulkanWindow(context, 1280, 800, 0, &config)
            , m_initial_path(initial_path)
            , m_requestedFormat(config.surfaceFormatIntent)
        {
        }

        ~VKAppWindow() override = default;

        void onDeviceReady() override
        {
            logSurfaceFormats(*this, m_requestedFormat);

            m_renderer = std::make_unique<VKRenderer>(*this);
            m_renderer->initialize();
            m_app = std::make_unique<AppView>(*this, *m_renderer);
            m_app->startup(m_initial_path);
        }

        void onSwapchainResize(VkExtent2D extent) override
        {
            MANGO_UNREFERENCED(extent);
            // Extent-sized resources are rebuilt in beginFrame() after beginDraw()
            // returns a correctly-sized image (ensureRenderTarget /
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
        IfapArgs args;
        CommandLineParser parser;
        configureParser(parser, args);

        if (!parser.parse(commands))
        {
            return;
        }

        if (args.info)
        {
            printEnable(Print::Info, true);
        }

        std::string_view initial_path;
        if (!parser.positionals().empty())
        {
            initial_path = parser.positionals()[0];
        }

        VulkanDeviceConfig deviceConfig;
        applyRecommendedSurfaceFormats(deviceConfig,
            args.sdr ? SurfaceFormatIntent::SDR : SurfaceFormatIntent::HDR);

        Instance instance = createVulkanInstance(args.validate);
        VulkanContext context(instance);
        VKAppWindow window(context, initial_path, deviceConfig);
        window.setTitle("iFap Image Viewer");

        EventLoopConfig config;
        config.mode = FrameMode::OnDemand;
        config.waitForFrame = true;

        EventLoop loop;
        loop.attach(window, config);
        loop.run();
    }

} // namespace ifap
