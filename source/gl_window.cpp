/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "window.hpp"
#include "app_view.hpp"
#include "render/gl/gl_renderer.hpp"

#include <mango/opengl/opengl.hpp>

/*
#ifdef MANGO_PLATFORM_WINDOWS
#include "resource/ifap.h"

static void setWindowIcon(Window& window)
{
    HINSTANCE instance = ::GetModuleHandle(NULL);
    HICON icon = ::LoadIcon(instance, MAKEINTRESOURCE(IDI_ICON1));
    if (!icon)
        return;
    HWND hwnd = WindowHandle(window).hwnd;
    ::SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)icon);
    ::SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
}
#endif
*/

namespace ifap
{
    using namespace mango;

    class GLAppWindow : public OpenGLContext
    {
    protected:
        GLRenderer m_renderer;
        AppView m_app;

    public:
        GLAppWindow(const OpenGLContext::Config& config, const CommandLine& commands)
            : OpenGLContext(1280, 800, 0, &config)
            , m_renderer(*this)
            , m_app(*this, m_renderer, [this] { toggleFullscreen(); })
        {
            m_renderer.initialize();
            m_app.startup(commands);
        }

        ~GLAppWindow() override = default;

        void onClose() override { m_app.onClose(); }
        void onMouseMove(int x, int y) override { m_app.onMouseMove(x, y); }
        void onMouseClick(int x, int y, MouseButton button, int count) override { m_app.onMouseClick(x, y, button, count); }
        void onKeyPress(Keycode code, u32 mask) override { m_app.onKeyPress(code, mask); }
        void onDropFiles(const FileIndex& index) override { m_app.onDropFiles(index); }
        void onIdle() override { m_app.onIdle(); }
        void onResize(int width, int height) override { m_app.onResize(width, height); }
        void onDraw() override { m_app.onDraw(); }
    };

    static OpenGLContext::Config createOpenGLConfig()
    {
        OpenGLContext::Config config;
        return config;
    }

    void runOpenGLApp(const CommandLine& commands)
    {
        const OpenGLContext::Config config = createOpenGLConfig();
        GLAppWindow window(config, commands);
        window.setTitle("iFap Image Viewer (OpenGL)");
        window.enterEventLoop();
    }

} // namespace ifap
