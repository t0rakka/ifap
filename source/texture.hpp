/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "indexer.hpp"
#include "render/render_backend.hpp"

namespace ifap
{
    using namespace mango;
    using namespace mango::filesystem;
    using namespace mango::image;

    struct DecodeTask
    {
        RenderBackend& renderer;

        std::unique_ptr<File> file;
        std::unique_ptr<ImageDecoder> decoder;
        std::unique_ptr<Bitmap> bitmap;
        std::unique_ptr<Bitmap> scaled_bitmap;
        ImageDecodeFuture future;

        GpuTexture texture;

        bool downscale = false;
        int downscale_width = 0;
        int downscale_height = 0;

        std::mutex mutex;
        std::vector<ImageDecodeRect> updates;
        float progress = 0.0f;

        explicit DecodeTask(RenderBackend& renderer);
        ~DecodeTask();

        std::vector<ImageDecodeRect> getUpdates();
    };

    class TextureCache
    {
    protected:
        RenderBackend& m_renderer;

        LRUCache<size_t, std::shared_ptr<DecodeTask>> m_cache { texture_cache_size };
        ImageFileIndexer m_indexer;

        std::shared_ptr<Path> m_current_path;

    public:
        explicit TextureCache(RenderBackend& renderer);
        ~TextureCache();

        operator const ImageFileIndexer& () const;

        size_t setCurrentPath(const std::string& name);
        GpuTexture getTexture(size_t index);
        bool syncTexture(size_t index, GpuTexture& texture);
        void update();

    protected:
        void uploadDownscaledPreview(DecodeTask& task);
    };

} // namespace ifap
