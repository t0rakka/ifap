/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "indexer.hpp"

namespace ifap
{
    using namespace mango;
    using namespace mango::filesystem;
    using namespace mango::image;
    using namespace mango::opengl;

    struct Texture
    {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
        GLenum format;
        GLenum type;
        bool linear;

        operator GLuint () const
        {
            return texture;
        }
    };

    struct DecodeTask
    {
        std::unique_ptr<File> file;
        std::unique_ptr<ImageDecoder> decoder;
        std::unique_ptr<Bitmap> bitmap;
        ImageDecodeFuture future;

        Texture texture;

        std::mutex mutex;
        std::vector<ImageDecodeRect> updates;
        float progress = 0.0f;

        DecodeTask();
        ~DecodeTask();

        std::vector<ImageDecodeRect> getUpdates();
    };

    class TextureCache
    {
    protected:
        LRUCache<size_t, std::shared_ptr<DecodeTask>> m_cache { texture_cache_size };
        ImageFileIndexer m_indexer;

        std::shared_ptr<Path> m_current_path;

        bool APPLE_client_storage = false;

    public:
        TextureCache(const OpenGLContext& context);
        ~TextureCache();

        operator const ImageFileIndexer& () const;

        size_t setCurrentPath(const std::string& name);
        Texture getTexture(size_t index);
        void update();
    };

} // namespace ifap
