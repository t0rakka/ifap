/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2026 Twilight 3D Finland Oy. All rights reserved.
*/
#include "debug_overlay.hpp"

#include <mango/core/system.hpp>
#include <mango/core/thread.hpp>

#include <algorithm>
#include <cmath>

namespace ifap
{
    using namespace mango;

    namespace
    {
        constexpr int kPanelMargin = 12;

        void pushHistorySample(std::deque<float>& history, float value)
        {
            history.push_back(value);
            while (history.size() > WorkerUtilizationOverlay::kMaxHistory)
            {
                history.pop_front();
            }
        }

        void plotLine(VKRenderer& renderer, int x0, int y0, int x1, int y1, int thickness,
                      float r, float g, float b, float a)
        {
            const int half = std::max(0, thickness / 2);
            int x = x0;
            int y = y0;
            const int dx = std::abs(x1 - x0);
            const int dy = std::abs(y1 - y0);
            const int sx = x0 < x1 ? 1 : -1;
            const int sy = y0 < y1 ? 1 : -1;
            int err = dx - dy;

            for (;;)
            {
                renderer.drawSolidRect(x - half, y - half, thickness, thickness, r, g, b, a);

                if (x == x1 && y == y1)
                {
                    break;
                }

                const int e2 = 2 * err;
                if (e2 > -dy)
                {
                    err -= dy;
                    x += sx;
                }
                if (e2 < dx)
                {
                    err += dx;
                    y += sy;
                }
            }
        }

        void plotSeries(VKRenderer& renderer, const std::deque<float>& history,
                        int panel_x, int panel_y, int panel_w, int panel_h,
                        float r, float g, float b, float a, int thickness)
        {
            if (history.empty())
                return;

            auto util_to_y = [&](float util) -> int
            {
                const float clamped = std::clamp(util, 0.0f, 1.0f);
                return panel_y + int(std::lround(clamped * float(panel_h - 1)));
            };

            const size_t count = history.size();
            const int slot_w = std::max(1, panel_w / int(WorkerUtilizationOverlay::kMaxHistory));
            const int offset_slots = int(WorkerUtilizationOverlay::kMaxHistory - count);

            auto sample_x = [&](size_t index)
            {
                return panel_x + (offset_slots + int(index)) * slot_w + slot_w / 2;
            };

            if (count == 1)
            {
                const int x = sample_x(0);
                const int y = util_to_y(history[0]);
                plotLine(renderer, x, y, x, y, thickness + 2, r, g, b, a);
                return;
            }

            for (size_t i = 1; i < count; ++i)
            {
                plotLine(renderer,
                    sample_x(i - 1), util_to_y(history[i - 1]),
                    sample_x(i), util_to_y(history[i]),
                    thickness, r, g, b, a);
            }

            plotLine(renderer,
                sample_x(count - 1), util_to_y(history.back()),
                sample_x(count - 1), util_to_y(history.back()),
                thickness + 2, r, g, b, std::min(1.0f, a + 0.06f));
        }
    }

    void WorkerUtilizationOverlay::reset()
    {
        m_primed = false;
        m_tp.clear();
        m_cpu.clear();
        m_gpu.clear();
    }

    void WorkerUtilizationOverlay::tick()
    {
        ThreadPool& pool = ThreadPool::getInstance();

        if (!m_primed)
        {
            pool.utilization();
            processCpuUtilization();
            gpuUtilization();
            m_primed = true;
            return;
        }

        const std::vector<float> util = pool.utilization();
        float tp = 0.0f;
        if (!util.empty())
            tp = *std::max_element(util.begin(), util.end());

        pushHistorySample(m_tp, std::clamp(tp, 0.0f, 1.0f));
        pushHistorySample(m_cpu, std::clamp(processCpuUtilization(), 0.0f, 1.0f));
        pushHistorySample(m_gpu, std::clamp(gpuUtilization(), 0.0f, 1.0f));
    }

    void WorkerUtilizationOverlay::draw(VKRenderer& renderer, int window_width, int window_height) const
    {
        if (window_width < 1 || window_height < 1)
        {
            return;
        }

        const int panel_w = std::max(96, window_width / 3);
        const int panel_h = std::max(96, window_height / 3);
        const int panel_x = std::max(0, window_width - kPanelMargin - panel_w);
        const int panel_y = std::max(0, window_height - kPanelMargin - panel_h);

        // drawSolidRect maps pixel y opposite to screen y within the panel: panel_y is
        // the visual floor, larger y is toward the ceiling.
        renderer.drawSolidRect(panel_x, panel_y, panel_w, panel_h, 0.0f, 0.0f, 0.0f, 0.65f);

        // Draw order: GPU under, CPU mid, TP on top (most familiar series).
        plotSeries(renderer, m_gpu, panel_x, panel_y, panel_w, panel_h,
            1.00f, 0.45f, 0.15f, 0.22f, 2); // orange — adapter GPU
        plotSeries(renderer, m_cpu, panel_x, panel_y, panel_w, panel_h,
            0.35f, 0.70f, 1.00f, 0.20f, 2); // blue — process CPU
        plotSeries(renderer, m_tp, panel_x, panel_y, panel_w, panel_h,
            0.30f, 1.00f, 0.50f, 0.18f, 2); // green — thread pool
    }

} // namespace ifap
