/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include <mango/core/timer.hpp>
#include <mango/core/string.hpp>
#include <mango/core/system.hpp>
#include <mango/image/decoder.hpp>
#include "indexer.hpp"

namespace ifap
{
    using namespace mango;

    // -----------------------------------------------------------------------
    // ImageFileIndexer
    // -----------------------------------------------------------------------

    ImageFileIndexer::ImageFileIndexer()
    {
    }

    ImageFileIndexer::~ImageFileIndexer()
    {
        reset();
    }

    void ImageFileIndexer::reset()
    {
        stop();
        m_filenames.clear();
    }

    void ImageFileIndexer::folder(const Path& path, const std::string& prefix, int depth)
    {
        std::vector<std::string> names;

        // first pass: files
        for (auto& info : path)
        {
            if (m_stop)
            {
                return;
            }

            if (!info.isDirectory())
            {
                // Filter only decodable files
                std::string extension = filesystem::getExtension(info.name);
                if (!extension.empty() && mango::image::isImageDecoder(extension))
                {
                    std::string filename = mango::removePrefix(path.pathname() + info.name, prefix);
                    names.emplace_back(filename);
                }
            }
        }

        // sort names
        std::sort(names.begin(), names.end());

        // store names
        if (!names.empty())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_filenames.insert(m_filenames.end(), names.begin(), names.end());
        }

        // second pass: folders
        for (auto& info : path)
        {
            if (m_stop)
            {
                return;
            }

            if (info.isDirectory())
            {
                const int cost = info.isContainer();
                if (depth < 2)
                {
                    Path temp(path, info.name);
                    folder(temp, prefix, depth + cost);
                }
            }
        }
    }

    bool ImageFileIndexer::isRunning() const
    {
        return m_running;
    }

    void ImageFileIndexer::start(const std::string& pathname)
    {
        reset();

        m_running = true;
        m_stop = false;

        m_queue.enqueue([this, pathname]
        {
            u64 time0 = mango::Time::ms();
            printLine("Indexer: start.");

            Path path(pathname);
            folder(path, pathname, 0);

            u64 time1 = mango::Time::ms();
            printLine("Indexer: complete {} ms.", time1 - time0);

            m_running = false;
        });
    }

    void ImageFileIndexer::stop()
    {
        m_stop = true;
        m_queue.cancel();
        m_queue.wait();
    }

    size_t ImageFileIndexer::size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_filenames.size();
    }

    std::string ImageFileIndexer::operator [] (size_t index) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_filenames[index];
    }

} // namespace ifap
