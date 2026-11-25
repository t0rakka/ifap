/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2026 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "render/vk/vk_renderer.hpp"

#include <deque>

namespace ifap
{

    // Rolling utilization HUD: thread-pool peak, process CPU, adapter GPU.
    // All series are pull-sampled at kSampleHz — no dedicated tracker thread.
    class WorkerUtilizationOverlay
    {
    public:
        static constexpr int kSampleHz = 10;
        static constexpr int kHistorySeconds = 30;
        static constexpr size_t kMaxHistory = size_t(kSampleHz * kHistorySeconds);

        void reset();
        void tick();
        void draw(VKRenderer& renderer, int window_width, int window_height) const;

    private:
        bool m_primed = false;
        std::deque<float> m_tp;
        std::deque<float> m_cpu;
        std::deque<float> m_gpu;
    };

} // namespace ifap
