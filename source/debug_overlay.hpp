/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "render/vk/vk_renderer.hpp"

#include <deque>

namespace ifap
{

    // Rolling peak thread-pool utilization (max busy fraction across workers per pulse).
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
        std::deque<float> m_aggregate;
    };

} // namespace ifap
