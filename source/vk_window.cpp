/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "window.hpp"
#include "app_view.hpp"
#include "render/vk/vk_renderer.hpp"

#include <mango/vulkan/vulkan.hpp>

namespace ifap
{
    using namespace mango;
    using namespace mango::filesystem;
    using namespace mango::vulkan;

    class VKAppWindow : public VulkanWindow
    {
    protected:
        VKRenderer m_renderer;
        AppView m_app;

    public:
        VKAppWindow(Instance& instance, const CommandLine& commands)
            : VulkanWindow(instance, 1280, 800, 0)
            , m_renderer(*this, instance, m_surface)
            , m_app(*this, m_renderer)
        {
            m_renderer.initialize();
            m_app.startup(commands);
            setVisible(true);
        }

        ~VKAppWindow() override = default;

        void onClose() override { m_app.onClose(); }
        void onMouseMove(int x, int y) override { m_app.onMouseMove(x, y); }
        void onMouseClick(int x, int y, MouseButton button, int count) override { m_app.onMouseClick(x, y, button, count); }
        void onKeyPress(Keycode code, u32 mask) override { m_app.onKeyPress(code, mask); }
        void onDropFiles(const FileIndex& index) override { m_app.onDropFiles(index); }
        void onIdle() override { m_app.onIdle(); }
        void onResize(int width, int height) override { m_app.onResize(width, height); }
        void onDraw() override { m_app.onDraw(); }
    };

    static Instance createVulkanInstance()
    {
        InstanceExtensionProperties instanceExtensionProperties;

        std::vector<const char*> enabledLayers;
        //enabledLayers.push_back("VK_LAYER_KHRONOS_validation");

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
        Instance instance = createVulkanInstance();
        VKAppWindow window(instance, commands);
        window.setTitle("iFap Image Viewer");
        window.enterEventLoop();
    }

} // namespace ifap
