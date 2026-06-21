/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "gl_renderer.hpp"

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
        GLint componentType;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
        if (componentType != GL_FLOAT)
        {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
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

        float32x2 size;
        size.x = float(std::max(1, request.width));
        size.y = float(std::max(1, request.height));

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
                m_program_bicubic.uniform_texture_scale.set(1.0f / size.x, 1.0f / size.y);
                m_program_bicubic.uniform_scale.set(request.intensity);

                glVertexAttribPointer(m_program_bicubic.attribute_position, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void*)0);
                glEnableVertexAttribArray(m_program_bicubic.attribute_position);
                break;
        }

        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(0));
    }

    void GLRenderer::createShaders()
    {
        static const char* vertex_shader_source = R"(
            #version 330

            uniform vec4 uTransform = vec4(0.0, 0.0, 1.0, 1.0);
            in vec2 inPosition;

            out vec2 texcoord;

            void main()
            {
                texcoord = inPosition * vec2(0.5, -0.5) + vec2(0.5);
                gl_Position = vec4((inPosition + uTransform.xy) * uTransform.zw, 0.0, 1.0);
            }
        )";

        static const char* fragment_shader_source = R"(
            #version 330

            uniform sampler2D uTexture;
            uniform float uScale;
            in vec2 texcoord;

            out vec4 outFragment0;

            void main()
            {
                vec4 color = texture(uTexture, texcoord);
                outFragment0 = color * uScale;
            }
        )";

        m_program_bilinear.program = opengl::createProgram(vertex_shader_source, fragment_shader_source);

        m_program_bilinear.uniform_transform.location = m_program_bilinear.getUniformLocation("uTransform");
        m_program_bilinear.uniform_texture.location = m_program_bilinear.getUniformLocation("uTexture");
        m_program_bilinear.uniform_scale.location = m_program_bilinear.getUniformLocation("uScale");
        m_program_bilinear.attribute_position = m_program_bilinear.getAttribLocation("inPosition");

        static const char* fragment_shader_source_bicubic = R"(
            #version 330

            vec4 cubic(float v)
            {
                vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;
                vec4 s = n * n * n;
                float x = s.x;
                float y = s.y - 4.0 * s.x;
                float z = s.z - 4.0 * s.y + 6.0 * s.x;
                float w = 6.0 - x - y - z;
                return vec4(x, y, z, w);
            }

            vec4 texture_filter(sampler2D uTexture, vec2 texcoord, vec2 texscale)
            {
                texcoord /= texscale;
                texcoord -= vec2(0.5, 0.5f);

                float fx = fract(texcoord.x);
                float fy = fract(texcoord.y);
                texcoord.x -= fx;
                texcoord.y -= fy;

                vec4 cx = cubic(fx);
                vec4 cy = cubic(fy);

                vec4 c = vec4(texcoord.x - 0.5, texcoord.x + 1.5, texcoord.y - 0.5, texcoord.y + 1.5);
                vec4 s = vec4(cx.x + cx.y, cx.z + cx.w, cy.x + cy.y, cy.z + cy.w);
                vec4 offset = c + vec4(cx.y, cx.w, cy.y, cy.w) / s;

                vec4 sample0 = texture(uTexture, vec2(offset.x, offset.z) * texscale);
                vec4 sample1 = texture(uTexture, vec2(offset.y, offset.z) * texscale);
                vec4 sample2 = texture(uTexture, vec2(offset.x, offset.w) * texscale);
                vec4 sample3 = texture(uTexture, vec2(offset.y, offset.w) * texscale);

                float sx = s.x / (s.x + s.y);
                float sy = s.z / (s.z + s.w);

                return mix(mix(sample3, sample2, sx), mix(sample1, sample0, sx), sy);
            }

            uniform sampler2D uTexture;
            uniform vec2 uTexScale;
            uniform float uScale;
            in vec2 texcoord;

            out vec4 outFragment0;

            void main()
            {
                vec4 color = texture_filter(uTexture, texcoord, uTexScale);
                outFragment0 = color * uScale;
            }
        )";

        m_program_bicubic.program = opengl::createProgram(vertex_shader_source, fragment_shader_source_bicubic);

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

} // namespace ifap
