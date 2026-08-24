/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "debug_overlay.hpp"

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
    }

    void WorkerUtilizationOverlay::reset()
    {
        m_primed = false;
        m_aggregate.clear();
    }

    void WorkerUtilizationOverlay::tick()
    {
        ThreadPool& pool = ThreadPool::getInstance();

        if (!m_primed)
        {
            pool.utilization();
            m_primed = true;
            return;
        }

        const std::vector<float> util = pool.utilization();
        if (util.empty())
        {
            return;
        }

        // Peak worker duty cycle in the sample window (0–1). Mean across workers
        // stays low when only a subset is busy; max shows decode bursts better.
        const float peak = *std::max_element(util.begin(), util.end());
        pushHistorySample(m_aggregate, std::clamp(peak, 0.0f, 1.0f));
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
        auto util_to_y = [&](float util) -> int
        {
            const float clamped = std::clamp(util, 0.0f, 1.0f);
            return panel_y + int(std::lround(clamped * float(panel_h - 1)));
        };

        renderer.drawSolidRect(panel_x, panel_y, panel_w, panel_h, 0.0f, 0.0f, 0.0f, 0.65f);

        if (m_aggregate.size() >= 2)
        {
            const size_t count = m_aggregate.size();
            const int slot_w = std::max(1, panel_w / int(kMaxHistory));
            const int offset_slots = int(kMaxHistory - count);

            auto sample_x = [&](size_t index)
            {
                return panel_x + (offset_slots + int(index)) * slot_w + slot_w / 2;
            };

            for (size_t i = 1; i < count; ++i)
            {
                plotLine(renderer,
                    sample_x(i - 1), util_to_y(m_aggregate[i - 1]),
                    sample_x(i), util_to_y(m_aggregate[i]),
                    2, 0.30f, 1.0f, 0.50f, 0.18f);
            }

            plotLine(renderer,
                sample_x(count - 1), util_to_y(m_aggregate.back()),
                sample_x(count - 1), util_to_y(m_aggregate.back()),
                4, 0.38f, 1.0f, 0.58f, 0.24f);
        }
        else if (m_aggregate.size() == 1)
        {
            const int slot_w = std::max(1, panel_w / int(kMaxHistory));
            const int offset_slots = int(kMaxHistory - 1);
            const int x = panel_x + offset_slots * slot_w + slot_w / 2;
            const int y = util_to_y(m_aggregate[0]);
            plotLine(renderer, x, y, x, y, 4, 0.38f, 1.0f, 0.58f, 0.24f);
        }
    }

} // namespace ifap
