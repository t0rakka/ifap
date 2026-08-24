/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "app_view.hpp"

#include <chrono>
#include <future>
#include <string>

namespace ifap
{

    AppView::AppView(Window& window, VKRenderer& renderer)
        : m_window(window)
        , m_renderer(renderer)
        , m_texture_cache(renderer,
            [this] { requestRedraw(); },
            [this] { return isExitRequested(); })
    {
        resetTransformation();
    }

    AppView::~AppView()
    {
        shutdown();
    }

    bool AppView::isExitRequested() const
    {
        if (m_shutdown)
        {
            return true;
        }

        // Abort background work only after the event loop has actually run. VulkanWindow
        // calls onFrame() from presentInitialFrame() before running is set, so treating
        // !isRunning() as exit there would abort the first image prepare/decode.
        return m_event_loop_running && !m_window.isRunning();
    }

    void AppView::shutdown()
    {
        if (m_shutdown)
        {
            return;
        }

        m_shutdown = true;
        m_texture_cache.shutdown();
    }

    void AppView::startup(std::string_view initial_path, bool debug)
    {
        m_left_time = 0;
        m_right_time = 0;
        m_debug = debug;
        m_debug_overlay.reset();
        m_debug_next_sample_ms = 0;

        if (m_debug)
        {
            requestRedraw();
            scheduleDebugFrame();
        }

        if (!initial_path.empty())
        {
            FileIndex index;
            index.emplace(std::string(initial_path));
            onDropFiles(index);
        }
    }

    void AppView::commitNavigation(size_t index)
    {
        const ImageFileIndexer& indexer = m_texture_cache;
        const size_t count = indexer.size();
        if (!count)
        {
            return;
        }

        m_current_index = modulo(index, count);
        m_nav_pending = false;
        m_pending_nav_index = m_current_index;
        m_last_nav_commit_ms = mango::Time::ms();
        m_current_task = m_texture_cache.getTexture(m_current_index, true);
        resetTransformation();
        m_awaiting_display = true;
    }

    void AppView::tryCommitNavigation(bool force)
    {
        if (!m_nav_pending)
        {
            return;
        }

        const u64 now = mango::Time::ms();
        const bool due = (now - m_last_nav_commit_ms) >= nav_latch_interval_ms;
        const bool quiet = (now - m_last_nav_input_ms) >= nav_latch_interval_ms;
        if (!force && !due && !quiet)
        {
            return;
        }

        if (m_pending_nav_index == m_current_index)
        {
            if (force || quiet)
            {
                m_nav_pending = false;
            }
            return;
        }

        commitNavigation(m_pending_nav_index);
    }

    void AppView::nextImage(int direction, bool latch)
    {
        if (!direction)
        {
            return;
        }

        const ImageFileIndexer& indexer = m_texture_cache;
        const size_t count = indexer.size();
        if (!count)
        {
            return;
        }

        m_texture_cache.setPrefetchDirection(direction);
        m_last_nav_input_ms = mango::Time::ms();

        if (latch)
        {
            const size_t base = m_nav_pending ? m_pending_nav_index : m_current_index;
            m_pending_nav_index = modulo(base + direction, count);
            m_nav_pending = true;
            tryCommitNavigation(false);
        }
        else
        {
            const size_t index = modulo(m_current_index + direction, count);
            commitNavigation(index);
        }
    }

    void AppView::resetTransformation()
    {
        m_translate = float32x2(0.0f, 0.0f);
        m_scale = 1.0f;
    }

    float32x2 AppView::computeAspect() const
    {
        int32x2 window_size = m_window.getWindowSize();
        window_size.x = std::max(1, window_size.x);
        window_size.y = std::max(1, window_size.y);

        int32x2 image_size;
        image_size.x = std::max(1, m_current_task ? m_current_task->texture.width : 1);
        image_size.y = std::max(1, m_current_task ? m_current_task->texture.height : 1);

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

    float32x2 AppView::computeTranslate() const
    {
        int32x2 window_size = m_window.getWindowSize();
        window_size.x = std::max(1, window_size.x);
        window_size.y = std::max(1, window_size.y);

        float dx = m_mouse_translate.delta.x / float(window_size.x) * 2.0f;
        float dy = m_mouse_translate.delta.y / float(window_size.y) * 2.0f;
        return float32x2(dx, dy);
    }

    float AppView::computeScale() const
    {
        int32x2 window_size = m_window.getWindowSize();
        window_size.y = std::max(1, window_size.y);

        float s = m_scale - m_scale * (m_mouse_scale.delta.y / float(window_size.y) * 4.0f);
        s = std::max(0.3f, s);
        return s;
    }

    ImageDrawRequest AppView::makeDrawRequest() const
    {
        float32x2 aspect = computeAspect();
        float32x2 scale = aspect * computeScale();
        float32x2 translate = m_translate + computeTranslate() / scale;

        ImageDrawRequest request;
        if (m_current_task)
        {
            const GpuTexture& texture = m_current_task->texture;
            request.texture = texture.handle;
            request.width = texture.sample_width;
            request.height = texture.sample_height;
            request.linear = texture.linear;
            request.needs_tonemap = texture.needs_tonemap;
        }
        request.translate = translate;
        request.scale = scale;
        request.filter = m_texture_filter;

        return request;
    }

    void AppView::onClose()
    {
        shutdown();
    }

    void AppView::requestRedraw()
    {
        if (isExitRequested())
        {
            return;
        }

        m_window.invalidate();
    }

    void AppView::scheduleNextFrame()
    {
        if (isExitRequested())
        {
            return;
        }

        // OnDemand idles with WAIT_INFINITE until an OS event arrives. Worker-thread
        // invalidate/wake is not reliable on macOS (mouse click "fixes" a gray window
        // because it delivers a real event). Arm a timed wake so we keep pumping until
        // the image is actually on screen.
        const double hz = m_window.getDisplayRefreshRate();
        const double interval = hz > 0.0 ? (1.0 / hz) : (1.0 / 60.0);
        m_window.requestFrameIn(interval);
    }

    void AppView::scheduleDebugFrame()
    {
        if (isExitRequested())
        {
            return;
        }

        m_window.requestFrameIn(1.0 / double(WorkerUtilizationOverlay::kSampleHz));
    }

    bool AppView::isContentDisplayed() const
    {
        if (!m_current_task)
        {
            return false;
        }

        const DecodeTask& task = *m_current_task;

        if (task.prepare_state != PrepareState::Ready || !task.gpu_texture_ready)
        {
            return false;
        }

        if (!task.texture.handle || !m_renderer.isTextureLayoutReady(task.texture.handle))
        {
            return false;
        }

        if (!task.content_uploaded)
        {
            return false;
        }

        if (task.hasPendingUpdates() || task.isDecoding())
        {
            return false;
        }

        if (!m_renderer.isTextureUploadComplete(task.texture.handle))
        {
            return false;
        }

        if (task.present_settle_frames > 0)
        {
            return false;
        }

        return true;
    }

    bool AppView::needsContinuousUpdate() const
    {
        if (isExitRequested())
        {
            return false;
        }

        if (m_awaiting_display)
        {
            return true;
        }

        if (m_mouse_translate.enable || m_mouse_scale.enable)
        {
            return true;
        }

        if (m_window.isKeyPressed(KEYCODE_LEFT) || m_window.isKeyPressed(KEYCODE_Q) ||
            m_window.isKeyPressed(KEYCODE_RIGHT) || m_window.isKeyPressed(KEYCODE_W))
        {
            return true;
        }

        if (m_current_task)
        {
            if (m_current_task->prepare_state != PrepareState::Ready)
            {
                return true;
            }

            if (!m_current_task->gpu_texture_ready || m_current_task->hasPendingUpdates())
            {
                return true;
            }

            if (m_current_task->texture.handle &&
                !m_renderer.isTextureLayoutReady(m_current_task->texture.handle))
            {
                return true;
            }

            if (m_current_task->texture.handle &&
                !m_renderer.isTextureUploadComplete(m_current_task->texture.handle))
            {
                return true;
            }

            if (m_current_task->present_settle_frames > 0)
            {
                return true;
            }

            if (m_current_task->future.valid() &&
                m_current_task->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                return true;
            }
        }

        return false;
    }

    void AppView::onMouseMove(int x, int y)
    {
        m_mouse_translate.update(x, y);
        m_mouse_scale.update(x, y);

        if (m_mouse_translate.enable || m_mouse_scale.enable)
        {
            requestRedraw();
        }
    }

    void AppView::onMouseClick(int x, int y, MouseButton button, int count)
    {
        switch (button)
        {
            case MOUSEBUTTON_LEFT:
                switch (count)
                {
                case 0:
                    m_translate += computeTranslate() / (computeAspect() * computeScale());
                    m_mouse_translate.end();
                    requestRedraw();
                    break;

                case 1:
                    m_mouse_translate.begin(x, y);
                    requestRedraw();
                    break;

                case 2:
                    m_window.toggleFullscreen();
                    break;

                default:
                    break;
                }
                break;

            case MOUSEBUTTON_RIGHT:
                if (count)
                {
                    m_mouse_scale.begin(x, y);
                    requestRedraw();
                }
                else
                {
                    m_scale = computeScale();
                    m_mouse_scale.end();
                    requestRedraw();
                }
                break;

            case MOUSEBUTTON_WHEEL:
                m_scale += count / 120.0f * m_scale / 6.0f;
                m_scale = std::max(0.3f, m_scale);
                requestRedraw();
                break;

            default:
                break;
        }
    }

    void AppView::onKeyPress(Keycode code, u32 mask)
    {
        MANGO_UNREFERENCED(mask);

        switch (code)
        {
            case KEYCODE_ESC:
                shutdown();
                m_window.requestQuit();
                break;

            case KEYCODE_F:
                m_window.toggleFullscreen();
                break;

            case KEYCODE_Q:
            case KEYCODE_LEFT:
                m_left_time = mango::Time::ms();
                nextImage(-1);
                requestRedraw();
                break;

            case KEYCODE_W:
            case KEYCODE_RIGHT:
                m_right_time = mango::Time::ms();
                nextImage(1);
                requestRedraw();
                break;

            case KEYCODE_1:
                m_texture_filter = TextureFilter::NEAREST;
                requestRedraw();
                break;
            case KEYCODE_2:
                m_texture_filter = TextureFilter::BILINEAR;
                requestRedraw();
                break;
            case KEYCODE_3:
                m_texture_filter = TextureFilter::BICUBIC;
                requestRedraw();
                break;

            default:
                break;
        }
    }

    void AppView::onDropFiles(const FileIndex& fileIndex)
    {
        if (fileIndex.empty())
        {
            return;
        }

        auto& object = fileIndex[0];

        size_t index = m_texture_cache.setCurrentPath(object.name);
        if (index != -1u)
        {
            m_nav_pending = false;
            commitNavigation(index);
            requestRedraw();
            m_window.dispatchFrame();
        }
    }

    void AppView::onResize(int width, int height)
    {
        MANGO_UNREFERENCED(width);
        MANGO_UNREFERENCED(height);

        // Do not touch GPU resources here. Wayland dispatches onResize from
        // dispatchPendingResize() before the swapchain extent is updated; rebuilding
        // the processing target against a stale extent crashes RADV. Swapchain-sized
        // resources are synced in VKRenderer::beginFrame() via ensureProcessingTarget()
        // after beginDraw() has acquired a correctly-sized image (same pattern as vkbasic's
        // onSwapchainResize, which is driven from beginDraw(), not Window::onResize).
    }

    void AppView::renderFrame()
    {
        const bool blend = !m_window.isKeyPressed(KEYCODE_B);
        const bool frame_active = m_renderer.beginFrame(0.06f, 0.06f, 0.06f, 1.0f, blend);

        if (frame_active && m_current_task && m_current_task->texture)
        {
            m_renderer.drawImage(makeDrawRequest());
        }

        if (frame_active && m_debug)
        {
            m_debug_overlay.draw(m_renderer, m_renderer.swapchainWidth(), m_renderer.swapchainHeight());
        }

        m_renderer.endFrame();
    }

    void AppView::onFrame(const FrameInfo& info)
    {
        MANGO_UNREFERENCED(info);

        m_loop_active = true;

        if (m_window.isRunning())
        {
            m_event_loop_running = true;
        }

        if (!m_window.isRunning())
        {
            // presentInitialFrame() invokes onFrame() before the event loop sets running.
            return;
        }

        const ImageFileIndexer& indexer = m_texture_cache;

        if (indexer.size() > 0 && m_current_index < indexer.size())
        {
            std::string filename = indexer[m_current_index];
            std::string title = fmt::format("[{} / {}] {}",
                m_current_index + 1, indexer.size(), filename);
            m_window.setTitle(title);
        }
        else
        {
            m_window.setTitle("iFap Image Viewer");
        }

        u64 current_time = mango::Time::ms();

        const bool nav_left = m_window.isKeyPressed(KEYCODE_LEFT) || m_window.isKeyPressed(KEYCODE_Q);
        const bool nav_right = m_window.isKeyPressed(KEYCODE_RIGHT) || m_window.isKeyPressed(KEYCODE_W);

        if (nav_left)
        {
            if (current_time - m_left_time > repeat_treshold)
            {
                m_left_time = current_time - (repeat_treshold - repeat_delay);
                nextImage(-1, true);
            }
        }

        if (nav_right)
        {
            if (current_time - m_right_time > repeat_treshold)
            {
                m_right_time = current_time - (repeat_treshold - repeat_delay);
                nextImage(1, true);
            }
        }

        if (!nav_left && !nav_right)
        {
            tryCommitNavigation(true);
        }

        // Input and texture work before swapchain acquire so event handling stays
        // responsive even when the GPU is busy decoding/uploading.
        const bool texture_progress = m_texture_cache.update(m_current_index, m_current_task);

        renderFrame();

        if (m_debug)
        {
            const u64 now = mango::Time::ms();
            if (m_debug_next_sample_ms == 0 || now >= m_debug_next_sample_ms)
            {
                m_debug_overlay.tick();
                m_debug_next_sample_ms = now + 1000 / WorkerUtilizationOverlay::kSampleHz;
            }

            scheduleDebugFrame();
            requestRedraw();
        }

        if (m_current_task && m_current_task->present_settle_frames > 0)
        {
            --m_current_task->present_settle_frames;
        }

        if (m_awaiting_display && isContentDisplayed())
        {
            m_awaiting_display = false;
        }

        if (m_awaiting_display)
        {
            scheduleNextFrame();
        }

        if (texture_progress || needsContinuousUpdate())
        {
            requestRedraw();
        }
    }

} // namespace ifap
