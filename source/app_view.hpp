/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "texture.hpp"
#include "render/vk/vk_renderer.hpp"
#include "window.hpp"

#include <mango/window/window.hpp>

namespace ifap
{
    using namespace mango;
    using namespace mango::math;
    using namespace mango::image;

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

    class AppView
    {
    protected:
        Window& m_window;
        VKRenderer& m_renderer;

        TextureCache m_texture_cache;

        TextureFilter m_texture_filter = TextureFilter::BILINEAR;

        MouseCapture m_mouse_translate;
        MouseCapture m_mouse_scale;

        float32x2 m_translate;
        float m_scale;

        u64 m_left_time = 0;
        u64 m_right_time = 0;

        std::shared_ptr<DecodeTask> m_current_task;
        size_t m_current_index = 0;

        void nextImage(int direction);
        void resetTransformation();

        float32x2 computeAspect() const;
        float32x2 computeTranslate() const;
        float computeScale() const;

        ImageDrawRequest makeDrawRequest() const;

    public:
        AppView(Window& window, VKRenderer& renderer);
        ~AppView();

        void startup(const CommandLine& commands);

        void onClose();
        void onMouseMove(int x, int y);
        void onMouseClick(int x, int y, MouseButton button, int count);
        void onKeyPress(Keycode code, u32 mask);
        void onDropFiles(const FileIndex& index);
        void onIdle();
        void onResize(int width, int height);
        void onDraw();
    };

} // namespace ifap
