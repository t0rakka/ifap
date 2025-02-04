/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include <mango/core/thread.hpp>
#include <mango/filesystem/path.hpp>

namespace ifap
{

    class ImageFileIndexer
    {
    protected:
        using Path = mango::filesystem::Path;

        mango::SerialQueue m_queue;
        std::atomic<bool> m_running { false };
        std::atomic<bool> m_stop { false };
        mutable std::mutex m_mutex;

        std::vector<std::string> m_filenames;

        void reset();
        void folder(const Path& path, const std::string& prefix, int depth);

    public:
        ImageFileIndexer();
        ~ImageFileIndexer();

        bool isRunning() const;

        void start(const std::string& pathname);
        void stop();

        size_t size() const;
        std::string operator [] (size_t index) const;
    };

} // namespace ifap
