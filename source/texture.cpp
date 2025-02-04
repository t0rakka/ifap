/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "texture.hpp"

namespace ifap
{

    // -----------------------------------------------------------------------
    // DecodeTask
    // -----------------------------------------------------------------------

    DecodeTask::DecodeTask()
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

        if (texture.texture)
        {
            glDeleteTextures(1, &texture.texture);
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

    TextureCache::TextureCache(const OpenGLContext& context)
    {
        APPLE_client_storage = context.ext.APPLE_client_storage;

        if (APPLE_client_storage)
        {
            printLine("APPLE_client_storage : enabled.");
        }
    }

    TextureCache::~TextureCache()
    {
    }

    TextureCache::operator const ImageFileIndexer& () const
    {
        return m_indexer;
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

    Texture TextureCache::getTexture(size_t index)
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
            std::shared_ptr<DecodeTask> task = std::make_shared<DecodeTask>();

            std::string filename = m_indexer[index];

            task->file = std::make_unique<File>(*m_current_path, filename);
            task->decoder = std::make_unique<ImageDecoder>(*task->file, filename);
            ImageHeader header = task->decoder->header();

            printLine("{}: {} x {}", filename, header.width, header.height);
            if (!header.width || !header.height)
            {
                return { 0, 0, 0 };
            }

            Texture& texture = task->texture;

            Format format;
            GLenum internalFormat;

            if (header.format.isFloat())
            {
                // TODO: check the extensions that the driver can do float/half
                if (header.format.bits <= 64)
                {
                    format = Format(64, Format::FLOAT16, Format::RGBA, 16, 16, 16, 16);
                    internalFormat = GL_RGBA16F;
                    texture.format = GL_RGBA;
                    texture.type = GL_HALF_FLOAT;
                    texture.linear = true;
                }
                else
                {
                    format = Format(128, Format::FLOAT32, Format::RGBA, 32, 32, 32, 32);
                    internalFormat = GL_RGBA32F;
                    texture.format = GL_RGBA;
                    texture.type = GL_FLOAT;
                    texture.linear = true;
                }
            }
            else
            {
                format = Format(32, Format::UNORM, Format::RGBA, 8, 8, 8, 8);

                if (header.linear)
                {
                    internalFormat = GL_RGBA;
                    texture.linear = true;
                }
                else
                {
                    internalFormat = GL_SRGB8;
                    texture.linear = false;
                }

                texture.format = GL_RGBA;
                texture.type = GL_UNSIGNED_BYTE;
            }

            task->bitmap = std::make_unique<Bitmap>(header.width, header.height, format);
            task->updates.clear();
            task->progress = 0.0f;

            glGenTextures(1, &texture.texture);
            texture.width = header.width;
            texture.height = header.height;

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, task->texture);

            if (APPLE_client_storage)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_CACHED_APPLE);
                glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, header.width, header.height, 0,
                             texture.format, texture.type, task->bitmap->image);
            }
            else
            {
                glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, header.width, header.height, 0,
                             texture.format, texture.type, nullptr);
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

    void TextureCache::update()
    {
        for (auto& value : m_cache)
        {
            DecodeTask& task = *value.second;
            Texture& texture = task.texture;

            if (texture.texture)
            {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);

                std::vector<ImageDecodeRect> updates = task.getUpdates();

                for (auto rect : updates)
                {
                    if (task.bitmap->width == rect.width)
                    {
                        u8* image = task.bitmap->address(rect.x, rect.y);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.width, rect.height,
                                        texture.format, texture.type, image);
                    }
                    else
                    {
                        Surface source(*task.bitmap, rect.x, rect.y, rect.width, rect.height);
                        Bitmap temp(source);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                        glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.width, rect.height,
                                        texture.format, texture.type, temp.image);
                    }
                }
            }
        }
    }

    /* TODO:

    - PBO to make texture updates non-blocking
    - half/float image format support w/ HDR resolve
    - sRGB and Linear resolve
    - block compressed textures w/o decompression

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCompressedTexImage2D(GL_TEXTURE_2D, 0, internalFormat, header.width, header.height, 0, GLsizei(block.size), block.address);

    textureFormat.surfaceFormat = Format(128, Format::FLOAT32, Format::RGBA, 32, 32, 32, 32);
    textureFormat.internalFormat = GL_RGBA32F;
    textureFormat.format = GL_RGBA;
    textureFormat.type = GL_FLOAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);

    textureFormat.surfaceFormat = FORMAT_L8;
    textureFormat.internalFormat = GL_RED;
    textureFormat.format = GL_RED;
    textureFormat.type = GL_UNSIGNED_BYTE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);

    textureFormat.surfaceFormat = FORMAT_L8A8;
    textureFormat.internalFormat = GL_RG;
    textureFormat.format = GL_RG;
    textureFormat.type = GL_UNSIGNED_BYTE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);

    case Format::FLOAT16:
        if (context.core.texture_float || context.core.half_float)

    case Format::FLOAT32:
        if (context.core.texture_float)

    GLenum getCompressedFormat(OpenGLContext& context, const ImageHeader& header)
    {
        GLenum format = 0;

		if (context.isCompressedTextureSupported(header.compression))
		{
			format = opengl::getTextureFormat(header.compression);
		}

        return format;
    }
    */

} // namespace ifap
