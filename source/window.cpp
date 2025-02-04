/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include <string>
#include <sstream>
#include "window.hpp"

#ifdef MANGO_PLATFORM_WINDOWS
#include "resource/ifap.h"
#endif

namespace ifap
{

    // -----------------------------------------------------------------------
    // AppWindow
    // -----------------------------------------------------------------------

    AppWindow::AppWindow(const OpenGLContext::Config& config, const CommandLine& commands)
        : OpenGLContext(1280, 800, 0, &config)
        , m_shaders()
        , m_texture_cache(*this)
    {
        setIcon();
        resetTransformation();

        GLint componentType;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
        if (componentType != GL_FLOAT)
        {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }

        printEnable(Print::Info, false);

        m_left_time = 0;
        m_right_time = 0;

        // process commands
        if (commands.size() > 1)
        {
            std::string filename = std::string(commands[1]);

            FileIndex index;
            index.emplace(filename);

            onDropFiles(index);
        }
    }

    AppWindow::~AppWindow()
    {
    }

    void AppWindow::setIcon()
    {
#ifdef MANGO_PLATFORM_WINDOWS
        HINSTANCE hinstance = ::GetModuleHandle(NULL);
        HICON hIcon = ::LoadIcon(hinstance, MAKEINTRESOURCE(IDI_ICON1));
        if (hIcon)
        {
            HWND hwnd = *this;
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessage(GetWindow(hwnd, GW_OWNER), WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessage(GetWindow(hwnd, GW_OWNER), WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            ::DestroyIcon(hIcon);
        }
#else
        // NOTE: setIcon() doesn't work on xlib when using GNOME
#endif
    }

    void AppWindow::nextImage(int direction)
    {
        if (direction)
        {
            const ImageFileIndexer& indexer = m_texture_cache;
            size_t count = indexer.size();
            if (!count)
            {
                // no files found
                return;
            }

            m_current_index = modulo(m_current_index + direction, count);
            m_current_texture = m_texture_cache.getTexture(m_current_index);

            if (texture_prefetch_size > 0)
            {
                for (size_t i = 0; i < texture_prefetch_size; ++i)
                {
                    size_t index = modulo(m_current_index + (i + 1) * direction, count);
                    m_texture_cache.getTexture(index);
                }
            }

            resetTransformation();
        }
    }

    void AppWindow::resetTransformation()
    {
        m_translate = float32x2(0.0f, 0.0f);
        m_scale = 1.0f;
    }

    float32x2 AppWindow::computeAspect() const
    {
        int32x2 window_size = getWindowSize();
        window_size.x = std::max(1, window_size.x);
        window_size.y = std::max(1, window_size.y);

        int32x2 image_size;
        image_size.x = std::max(1, m_current_texture.width);
        image_size.y = std::max(1, m_current_texture.height);

        float32x2 aspect;
        aspect.x = float(window_size.x) / float(image_size.x);
        aspect.y = float(window_size.y) / float(image_size.y);

        if (aspect.x < aspect.y)
        {
            aspect.y = aspect.x / aspect.y;
            aspect.x = 1.0f;
        }
        else
        {
            aspect.x = aspect.y / aspect.x;
            aspect.y = 1.0f;
        }

        return aspect;
    }

    float32x2 AppWindow::computeTranslate() const
    {
        int32x2 window_size = getWindowSize();
        window_size.x = std::max(1, window_size.x);
        window_size.y = std::max(1, window_size.y);
        float dx = m_mouse_translate.delta.x / float(window_size.x) * 2.0f;
        float dy = m_mouse_translate.delta.y / float(window_size.y) * 2.0f;
        return float32x2(dx, dy);
    }

    float AppWindow::computeScale() const
    {
        int32x2 window_size = getWindowSize();
        window_size.y = std::max(1, window_size.y);
        float s = m_scale - m_scale * (m_mouse_scale.delta.y / float(window_size.y) * 4.0f);
        s = std::max(0.3f, s);
        return s;
    }

    void AppWindow::onClose()
    {
    }

    void AppWindow::onMouseMove(int x, int y)
    {
        m_mouse_translate.update(x, y);
        m_mouse_scale.update(x, y);
    }

    void AppWindow::onMouseClick(int x, int y, MouseButton button, int count)
    {
        switch (button)
        {
            case MOUSEBUTTON_LEFT:
                switch (count)
                {
                case 0:
                    m_translate += computeTranslate() / (computeAspect() * computeScale());
                    m_mouse_translate.end();
                    break;

                case 1:
                    m_mouse_translate.begin(x, y);
                    break;

                case 2:
                    toggleFullscreen();
                    break;

                default:
                    break;
                }
                break;

            case MOUSEBUTTON_RIGHT:
                if (count)
                {
                    m_mouse_scale.begin(x, y);
                }
                else
                {
                    m_scale = computeScale();
                    m_mouse_scale.end();
                }
                break;

            case MOUSEBUTTON_WHEEL:
                m_scale += count / 120.0f * m_scale / 6.0f;
                m_scale = std::max(0.3f, m_scale);
                break;

            default:
                break;
        }
    }

    void AppWindow::onKeyPress(Keycode code, u32 mask)
    {
        switch (code)
        {
            case KEYCODE_ESC:
                breakEventLoop();
                break;

            case KEYCODE_F:
                toggleFullscreen();
                break;

            case KEYCODE_Q:
            case KEYCODE_LEFT:
                m_left_time = mango::Time::ms();
                nextImage(-1);
                break;

            case KEYCODE_W:
            case KEYCODE_RIGHT:
                m_right_time = mango::Time::ms();
                nextImage(1);
                break;

            case KEYCODE_H:
                m_hdr = !m_hdr;
                break;

            case KEYCODE_1:
                m_shaders.texture_filter = TextureFilter::NEAREST;
                break;
            case KEYCODE_2:
                m_shaders.texture_filter = TextureFilter::BILINEAR;
                break;
            case KEYCODE_3:
                m_shaders.texture_filter = TextureFilter::BICUBIC;
                break;

            default:
                break;
        }
    }

    void AppWindow::onDropFiles(const FileIndex& fileIndex)
    {
        if (fileIndex.empty())
        {
            return;
        }

        // select first dropped object
        auto& object = fileIndex[0];

        size_t index = m_texture_cache.setCurrentPath(object.name);
        if (index != -1u)
        {
            m_current_index = index;
            m_current_texture = m_texture_cache.getTexture(m_current_index);
            resetTransformation();
        }
    }

    void AppWindow::onIdle()
    {
        const ImageFileIndexer& indexer = m_texture_cache;

        if (indexer.size() > 0)
        {
            std::string filename = indexer[m_current_index];
            std::string title = fmt::format("[{} / {}] {}",
                m_current_index + 1, indexer.size(), filename);
            setTitle(title);
        }
        else
        {
            setTitle("iFap Image Viewer");
        }

        onDraw();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    void AppWindow::onResize(int width, int height)
    {
        glViewport(0, 0, width, height);
        glScissor(0, 0, width, height);
    }

    void AppWindow::onDraw()
    {
        u64 current_time = mango::Time::ms();

        const u64 repeat_treshold = 420;
        const u64 repeat_delay = 3;

        if (isKeyPressed(KEYCODE_LEFT) || isKeyPressed(KEYCODE_Q))
        {
            if (current_time - m_left_time > repeat_treshold)
            {
                m_left_time = current_time - (repeat_treshold - repeat_delay);
                nextImage(-1);
            }
        }

        if (isKeyPressed(KEYCODE_RIGHT) || isKeyPressed(KEYCODE_W))
        {
            if (current_time - m_right_time > repeat_treshold)
            {
                m_right_time = current_time - (repeat_treshold - repeat_delay);
                nextImage(1);
            }
        }

        glClearColor(0.06f, 0.06f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (isKeyPressed(KEYCODE_B))
            glDisable(GL_BLEND);
        else
            glEnable(GL_BLEND);

        m_texture_cache.update();

        GLuint texture = m_current_texture.texture;
        if (texture)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);

            float32x2 aspect = computeAspect();
            float32x2 scale = aspect * computeScale();
            float32x2 translate = m_translate + computeTranslate() / scale;

            float intensity = 1.0f;

            if (!m_current_texture.linear && m_hdr)
            {
                intensity = 2.0f;
            }

            m_shaders.draw(m_current_texture, intensity, translate, scale);
        }

        swapBuffers();
    }

} // namespace ifap
