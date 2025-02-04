/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "texture.hpp"
#include "shaders.hpp"

namespace ifap
{
    using namespace mango;
    using namespace mango::math;
    using namespace mango::image;

    using CommandLine = std::vector<std::string_view>;

    struct MouseCapture
    {
        bool enable;
        int32x2 origin;
        int32x2 delta;

        MouseCapture()
        {
            enable = false;
            origin = int32x2(0, 0);
            delta = int32x2(0, 0);
        }

        ~MouseCapture()
        {
        }

        void begin(int x, int y)
        {
            enable = true;
            origin = int32x2(x, y);
        }

        void end()
        {
            enable = false;
            origin = int32x2(0, 0);
            delta = int32x2(0, 0);
        }

        void update(int x, int y)
        {
            if (enable)
            {
                delta = int32x2(x, y) - origin;
            }
        }
    };

    class AppWindow : public OpenGLContext
    {
    protected:
        Shaders m_shaders;
        bool m_hdr = false;

        MouseCapture m_mouse_translate;
        MouseCapture m_mouse_scale;

        float32x2 m_translate;
        float m_scale;

        u64 m_left_time = 0;
        u64 m_right_time = 0;

        TextureCache m_texture_cache;
        Texture m_current_texture;
        size_t m_current_index = 0;

        void setIcon();
        void nextImage(int direction);
        void resetTransformation();

        float32x2 computeAspect() const;
        float32x2 computeTranslate() const;
        float computeScale() const;

    public:
        AppWindow(const OpenGLContext::Config& config, const CommandLine& commands);
        ~AppWindow();

        void onClose() override;
        void onMouseMove(int x, int y) override;
        void onMouseClick(int x, int y, mango::MouseButton button, int count) override;
        void onKeyPress(mango::Keycode code, u32 mask) override;
        void onDropFiles(const FileIndex& index) override;
        void onIdle() override;
        void onResize(int width, int height) override;
        void onDraw() override;
    };

} // namespace ifap

