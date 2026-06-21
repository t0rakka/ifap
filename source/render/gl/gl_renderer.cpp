/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "gl_renderer.hpp"
#include "../image_shaders.hpp"

namespace ifap
{
    using namespace mango;

    // -----------------------------------------------------------------------
    // GLRenderer
    // -----------------------------------------------------------------------

    GLRenderer::GLRenderer(OpenGLContext& context)
        : m_context(context)
    {
        m_apple_client_storage = context.ext.APPLE_client_storage;

        if (m_apple_client_storage)
        {
            printLine("APPLE_client_storage : enabled.");
        }

        createShaders();
        createGeometry();
    }

    GLRenderer::~GLRenderer()
    {
        destroyGeometry();

        glDeleteProgram(m_program_bilinear.program);
        glDeleteProgram(m_program_bicubic.program);
    }

    void GLRenderer::initialize()
    {
        GLint max_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
        m_max_texture_dimension = int(max_size);

        GLint componentType;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
        if (componentType != GL_FLOAT)
        {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
    }

    int GLRenderer::getMaxTextureDimension() const
    {
        return m_max_texture_dimension;
    }

    void GLRenderer::resize(int width, int height)
    {
        glViewport(0, 0, width, height);
        glScissor(0, 0, width, height);
    }

    void GLRenderer::beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend)
    {
        glClearColor(clear_r, clear_g, clear_b, clear_a);
        glClear(GL_COLOR_BUFFER_BIT);

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
    }

    GLenum GLRenderer::getInternalFormat(PixelFormat format)
    {
        switch (format)
        {
            case PixelFormat::RGBA8_UNORM: return GL_RGBA;
            case PixelFormat::RGBA8_SRGB:  return GL_SRGB8;
            case PixelFormat::RGBA16F:     return GL_RGBA16F;
            case PixelFormat::RGBA32F:     return GL_RGBA32F;
        }

        return GL_RGBA;
    }

    GLenum GLRenderer::getFormat(PixelFormat format)
    {
        MANGO_UNREFERENCED(format);
        return GL_RGBA;
    }

    GLenum GLRenderer::getType(PixelFormat format)
    {
        switch (format)
        {
            case PixelFormat::RGBA8_UNORM:
            case PixelFormat::RGBA8_SRGB:
                return GL_UNSIGNED_BYTE;

            case PixelFormat::RGBA16F:
                return GL_HALF_FLOAT;

            case PixelFormat::RGBA32F:
                return GL_FLOAT;
        }

        return GL_UNSIGNED_BYTE;
    }

    TextureHandle GLRenderer::createTexture(int width, int height, PixelFormat format, const void* initial_data)
    {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);

        if (m_apple_client_storage && initial_data)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_CACHED_APPLE);
            glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, getInternalFormat(format), width, height, 0,
                         getFormat(format), getType(format), initial_data);
        }
        else
        {
            glTexImage2D(GL_TEXTURE_2D, 0, getInternalFormat(format), width, height, 0,
                         getFormat(format), getType(format), initial_data);
        }

        return TextureHandle(texture);
    }

    void GLRenderer::uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                         int x, int y, int width, int height, const void* pixels)
    {
        if (!handle)
        {
            return;
        }

        GLuint texture = GLuint(handle);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height,
                        getFormat(format), getType(format), pixels);
    }

    void GLRenderer::destroyTexture(TextureHandle handle)
    {
        if (!handle)
        {
            return;
        }

        GLuint texture = GLuint(handle);
        glDeleteTextures(1, &texture);
    }

    void GLRenderer::drawImage(const ImageDrawRequest& request)
    {
        if (!request.texture)
        {
            return;
        }

        GLuint texture = GLuint(request.texture);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindVertexArray(m_vao);

        const float32x2 tex_scale = shaders::imageTexScale(request.width, request.height, true);
        const float32x2& translate = request.translate;
        const float32x2& scale = request.scale;

        switch (request.filter)
        {
            case TextureFilter::NEAREST:
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                glUseProgram(m_program_bilinear.program);
                m_program_bilinear.uniform_transform.set(translate.x, -translate.y, scale.x, scale.y);
                m_program_bilinear.uniform_texture.set(0);
                m_program_bilinear.uniform_texture_scale.set(tex_scale.x, tex_scale.y);
                m_program_bilinear.uniform_scale.set(request.intensity);

                glVertexAttribPointer(m_program_bilinear.attribute_position, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void*)0);
                glEnableVertexAttribArray(m_program_bilinear.attribute_position);
                break;

            case TextureFilter::BILINEAR:
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                glUseProgram(m_program_bilinear.program);
                m_program_bilinear.uniform_transform.set(translate.x, -translate.y, scale.x, scale.y);
                m_program_bilinear.uniform_texture.set(0);
                m_program_bilinear.uniform_texture_scale.set(tex_scale.x, tex_scale.y);
                m_program_bilinear.uniform_scale.set(request.intensity);

                glVertexAttribPointer(m_program_bilinear.attribute_position, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void*)0);
                glEnableVertexAttribArray(m_program_bilinear.attribute_position);
                break;

            case TextureFilter::BICUBIC:
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                glUseProgram(m_program_bicubic.program);
                m_program_bicubic.uniform_transform.set(translate.x, -translate.y, scale.x, scale.y);
                m_program_bicubic.uniform_texture.set(0);
                m_program_bicubic.uniform_texture_scale.set(tex_scale.x, tex_scale.y);
                m_program_bicubic.uniform_scale.set(request.intensity);

                glVertexAttribPointer(m_program_bicubic.attribute_position, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void*)0);
                glEnableVertexAttribArray(m_program_bicubic.attribute_position);
                break;
        }

        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(0));
    }

    void GLRenderer::createShaders()
    {
        const std::string vertex_shader_source = shaders::glVertexShader();
        const std::string fragment_shader_bilinear = shaders::glFragmentShaderBilinear();
        const std::string fragment_shader_bicubic = shaders::glFragmentShaderBicubic();

        m_program_bilinear.program = opengl::createProgram(vertex_shader_source, fragment_shader_bilinear);

        m_program_bilinear.uniform_transform.location = m_program_bilinear.getUniformLocation("uTransform");
        m_program_bilinear.uniform_texture.location = m_program_bilinear.getUniformLocation("uTexture");
        m_program_bilinear.uniform_texture_scale.location = m_program_bilinear.getUniformLocation("uTexScale");
        m_program_bilinear.uniform_scale.location = m_program_bilinear.getUniformLocation("uScale");
        m_program_bilinear.attribute_position = m_program_bilinear.getAttribLocation("inPosition");

        m_program_bicubic.program = opengl::createProgram(vertex_shader_source, fragment_shader_bicubic);

        m_program_bicubic.uniform_transform.location = m_program_bicubic.getUniformLocation("uTransform");
        m_program_bicubic.uniform_texture.location = m_program_bicubic.getUniformLocation("uTexture");
        m_program_bicubic.uniform_texture_scale.location = m_program_bicubic.getUniformLocation("uTexScale");
        m_program_bicubic.uniform_scale.location = m_program_bicubic.getUniformLocation("uScale");
        m_program_bicubic.attribute_position = m_program_bicubic.getAttribLocation("inPosition");
    }

    void GLRenderer::createGeometry()
    {
        static const GLfloat vertex_buffer_data [] =
        {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f
        };

        static const GLushort element_buffer_data [] =
        {
            0, 1, 2, 3
        };

        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);

        auto createBuffer = [] (GLenum target, const void* data, GLsizei size)
        {
            GLuint buffer;
            glGenBuffers(1, &buffer);
            glBindBuffer(target, buffer);
            glBufferData(target, size, data, GL_STATIC_DRAW);
            return buffer;
        };

        m_vertex_buffer = createBuffer(GL_ARRAY_BUFFER, vertex_buffer_data, sizeof(vertex_buffer_data));
        m_element_buffer = createBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer_data, sizeof(element_buffer_data));
    }

    void GLRenderer::destroyGeometry()
    {
        glDeleteVertexArrays(1, &m_vao);
        glDeleteBuffers(1, &m_vertex_buffer);
        glDeleteBuffers(1, &m_element_buffer);
    }

    void GLRenderer::endFrame()
    {
        m_context.swapBuffers();
    }

} // namespace ifap
