/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "texture.hpp"

#include <mango/image/bicubic.hpp>

#include <algorithm>

namespace ifap
{

    namespace
    {
        math::int32x2 computeDownscaleDimensions(int width, int height, int max_dimension)
        {
            if (width <= max_dimension && height <= max_dimension)
            {
                return math::int32x2(width, height);
            }

            const float scale = std::min(float(max_dimension) / float(width),
                                         float(max_dimension) / float(height));

            return math::int32x2(
                std::max(1, int(float(width) * scale)),
                std::max(1, int(float(height) * scale)));
        }

        void createPlaceholderTexture(DecodeTask& task, RenderBackend& renderer, int width, int height)
        {
            static const u8 placeholder_pixel[] = { 32, 32, 32, 255 };

            GpuTexture& texture = task.texture;
            texture.width = width;
            texture.height = height;
            texture.sample_width = 1;
            texture.sample_height = 1;
            texture.format = PixelFormat::RGBA8_UNORM;
            texture.linear = true;
            texture.handle = renderer.createTexture(1, 1, PixelFormat::RGBA8_UNORM, placeholder_pixel);
        }

    } // namespace

    // -----------------------------------------------------------------------
    // DecodeTask
    // -----------------------------------------------------------------------

    DecodeTask::DecodeTask(RenderBackend& renderer)
        : renderer(renderer)
    {
    }

    DecodeTask::~DecodeTask()
    {
        if (decoder)
        {
            decoder->cancel();
        }

        if (future.valid())
        {
            future.get();
        }

        if (texture.handle)
        {
            renderer.destroyTexture(texture.handle);
        }
    }

    std::vector<ImageDecodeRect> DecodeTask::getUpdates()
    {
        std::lock_guard lock(mutex);
        std::vector<ImageDecodeRect> temp;
        std::swap(temp, updates);
        return temp;
    }

    // -----------------------------------------------------------------------
    // TextureCache
    // -----------------------------------------------------------------------

    TextureCache::TextureCache(RenderBackend& renderer)
        : m_renderer(renderer)
    {
    }

    TextureCache::~TextureCache()
    {
    }

    TextureCache::operator const ImageFileIndexer& () const
    {
        return m_indexer;
    }

    void TextureCache::uploadDownscaledPreview(DecodeTask& task)
    {
        GpuTexture& texture = task.texture;

        if (!task.bitmap || !task.scaled_bitmap)
        {
            return;
        }

        const int dw = task.downscale_width;
        const int dh = task.downscale_height;

        u32_bicubic_blit(*task.scaled_bitmap, *task.bitmap,
            0.5f, 0.5f, float(dw) - 1.0f, float(dh) - 1.0f);

        if (!texture.handle)
        {
            texture.handle = m_renderer.createTexture(dw, dh, texture.format, task.scaled_bitmap->image);
            return;
        }

        m_renderer.uploadTextureRegion(texture.handle, texture.format,
            0, 0, dw, dh, task.scaled_bitmap->image);
    }

    size_t TextureCache::setCurrentPath(const std::string& name)
    {
        m_cache.clear();

        std::string filename = name;
        Path temp(filename);

        if (temp.isFile(filename))
        {
            if (Mapper::isCustomMapper(filename))
            {
                // the object was container file
                std::string pathname = filename + "/";
                m_current_path = std::make_shared<Path>(pathname);
                m_indexer.start(pathname);
                filename.clear();
            }
            else
            {
                // the object was a regular file
                if (!mango::image::isImageDecoder(filename))
                {
                    // not supported fileformat
                    return -1u;
                }

                std::string pathname = getPath(filename);
                m_current_path = std::make_shared<Path>(pathname);
                m_indexer.start(pathname);
                filename = removePath(filename);
            }
        }
        else
        {
            std::string pathname = getPath(filename);
            m_current_path = std::make_shared<Path>(pathname);
            m_indexer.start(pathname);
            filename.clear();
        }

        size_t m_current_index = -1u;

        if (filename.empty())
        {
            auto getIndexOfFirstFile = [this] () -> size_t
            {
                bool done = false;
                bool last = false;

                while (!done)
                {
                    if (m_indexer.size() > 0)
                    {
                        // found a file in the path; that's the chosen one
                        return 0;
                    }

                    mango::Sleep::ms(50);

                    // this way we run extra iteration after the indexer is not running
                    // to get any last-moment writes the indexer might have done
                    done = last;
                    last = !m_indexer.isRunning();
                }

                // no files in current path
                return -1u;
            };

            m_current_index = getIndexOfFirstFile();
        }
        else
        {
            auto getIndexFromFilename = [this] (const std::string& filename) -> size_t
            {
                size_t position = 0;

                bool done = false;
                bool last = false;

                while (!done)
                {
                    for ( ; position < m_indexer.size(); ++position)
                    {
                        if (m_indexer[position] == filename)
                        {
                            // found a file with matching name in current path
                            return position;
                        }
                    }

                    mango::Sleep::ms(5);

                    // this way we run extra iteration after the indexer is not running
                    // to get any last-moment writes the indexer might have done
                    done = last;
                    last = !m_indexer.isRunning();
                }

                // no files in current path
                return -1u;
            };

            m_current_index = getIndexFromFilename(filename);
        }

        return m_current_index;
    }

    GpuTexture TextureCache::getTexture(size_t index)
    {
        auto entry = m_cache.get(index);
        if (entry)
        {
            // cache hit
            const DecodeTask& task = *entry.value();
            return task.texture;
        }
        else
        {
            // cache miss
            std::shared_ptr<DecodeTask> task = std::make_shared<DecodeTask>(m_renderer);

            std::string filename = m_indexer[index];

            task->file = std::make_unique<File>(*m_current_path, filename);
            task->decoder = std::make_unique<ImageDecoder>(*task->file, filename);
            ImageHeader header = task->decoder->header();

            printLine("{}: {} x {}", filename, header.width, header.height);
            if (!header.width || !header.height)
            {
                return {};
            }

            const int max_texture_dimension = m_renderer.getMaxTextureDimension();
            const bool needs_downscale = max_texture_dimension > 0 &&
                (header.width > max_texture_dimension || header.height > max_texture_dimension);

            if (needs_downscale && header.format.isFloat())
            {
                printLine(Print::Error, "{}: {} x {} exceeds GPU texture limit (max dimension: {}); float preview not supported yet",
                    filename, header.width, header.height, max_texture_dimension);

                createPlaceholderTexture(*task, m_renderer, header.width, header.height);

                m_cache.insert(index, task);
                return task->texture;
            }

            GpuTexture& texture = task->texture;

            Format format;
            PixelFormat pixel_format;

            if (header.format.isFloat())
            {
                // TODO: check the extensions that the driver can do float/half
                if (header.format.bits <= 64)
                {
                    format = Format(64, Format::FLOAT16, Format::RGBA, 16, 16, 16, 16);
                    pixel_format = PixelFormat::RGBA16F;
                    texture.linear = true;
                }
                else
                {
                    format = Format(128, Format::FLOAT32, Format::RGBA, 32, 32, 32, 32);
                    pixel_format = PixelFormat::RGBA32F;
                    texture.linear = true;
                }
            }
            else
            {
                format = Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8);

                if (header.linear)
                {
                    pixel_format = PixelFormat::RGBA8_UNORM;
                    texture.linear = true;
                }
                else
                {
                    pixel_format = PixelFormat::RGBA8_SRGB;
                    texture.linear = false;
                }
            }

            if (needs_downscale)
            {
                const math::int32x2 preview_size = computeDownscaleDimensions(
                    header.width, header.height, max_texture_dimension);

                task->downscale = true;
                task->downscale_width = preview_size.x;
                task->downscale_height = preview_size.y;

                printLine(Print::Info, "{}: {} x {} exceeds GPU limit ({}), preview {} x {}",
                    filename, header.width, header.height, max_texture_dimension,
                    task->downscale_width, task->downscale_height);

                task->scaled_bitmap = std::make_unique<Bitmap>(task->downscale_width, task->downscale_height, format);
            }

            task->bitmap = std::make_unique<Bitmap>(header.width, header.height, format);
            task->updates.clear();
            task->progress = 0.0f;

            texture.width = header.width;
            texture.height = header.height;
            texture.format = pixel_format;

            if (needs_downscale)
            {
                texture.sample_width = task->downscale_width;
                texture.sample_height = task->downscale_height;
            }
            else
            {
                texture.sample_width = header.width;
                texture.sample_height = header.height;

                const void* initial_data = nullptr;
                if (task->bitmap->image)
                {
                    initial_data = task->bitmap->image;
                }

                texture.handle = m_renderer.createTexture(header.width, header.height, pixel_format, initial_data);
            }

            // don't capture the shared_ptr because the callback lambda will hold a reference
            task->future = task->decoder->launch([task = task.get()] (const ImageDecodeRect& rect)
            {
                std::lock_guard lock(task->mutex);
                task->updates.push_back(rect);
                task->progress += rect.progress;
            }, *task->bitmap);

            m_cache.insert(index, task);
            return task->texture;
        }
    }

    bool TextureCache::syncTexture(size_t index, GpuTexture& texture)
    {
        auto entry = m_cache.get(index);
        if (!entry)
        {
            return false;
        }

        texture = entry.value()->texture;
        return true;
    }

    void TextureCache::update()
    {
        for (auto& value : m_cache)
        {
            DecodeTask& task = *value.second;
            GpuTexture& texture = task.texture;

            if (!task.bitmap)
            {
                continue;
            }

            const std::vector<ImageDecodeRect> updates = task.getUpdates();

            if (task.downscale)
            {
                if (!updates.empty() || !texture.handle)
                {
                    uploadDownscaledPreview(task);
                }

                continue;
            }

            if (!texture.handle)
            {
                continue;
            }

            for (auto rect : updates)
            {
                if (task.bitmap->width == rect.width)
                {
                    u8* image = task.bitmap->address(rect.x, rect.y);
                    m_renderer.uploadTextureRegion(texture.handle, texture.format,
                        rect.x, rect.y, rect.width, rect.height, image);
                }
                else
                {
                    Surface source(*task.bitmap, rect.x, rect.y, rect.width, rect.height);
                    Bitmap temp(source);
                    m_renderer.uploadTextureRegion(texture.handle, texture.format,
                        rect.x, rect.y, rect.width, rect.height, temp.image);
                }
            }
        }
    }

} // namespace ifap
