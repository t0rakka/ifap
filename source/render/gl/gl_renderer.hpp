/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "../render_backend.hpp"

#include <mango/opengl/opengl.hpp>

namespace ifap
{

    class GLRenderer : public RenderBackend
    {
    protected:
        mango::OpenGLContext& m_context;
        bool m_apple_client_storage = false;

        struct Uniform
        {
            GLint location;
        };

        struct Uniform1i : Uniform
        {
            void set(GLint v0) const
            {
                glUniform1i(location, v0);
            }
        };

        struct Uniform1f : Uniform
        {
            void set(GLfloat v) const
            {
                glUniform1f(location, v);
            }
        };

        struct Uniform2f : Uniform
        {
            void set(GLfloat v0, GLfloat v1) const
            {
                glUniform2f(location, v0, v1);
            }
        };

        struct Uniform4f : Uniform
        {
            void set(GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) const
            {
                glUniform4f(location, v0, v1, v2, v3);
            }
        };

        struct Program
        {
            GLuint program = 0;

            GLint getUniformLocation(const GLchar* name) const
            {
                return glGetUniformLocation(program, name);
            }

            GLint getAttribLocation(const GLchar* name) const
            {
                return glGetAttribLocation(program, name);
            }
        };

        struct ProgramBilinear : Program
        {
            Uniform4f uniform_transform;
            Uniform1i uniform_texture;
            Uniform1f uniform_scale;
            GLint attribute_position;
        };

        struct ProgramBicubic : Program
        {
            Uniform4f uniform_transform;
            Uniform1i uniform_texture;
            Uniform2f uniform_texture_scale;
            Uniform1f uniform_scale;
            GLint attribute_position;
        };

        ProgramBilinear m_program_bilinear;
        ProgramBicubic m_program_bicubic;

        GLuint m_vao = 0;
        GLuint m_vertex_buffer = 0;
        GLuint m_element_buffer = 0;

        void createShaders();
        void createGeometry();
        void destroyGeometry();

        static GLenum getInternalFormat(PixelFormat format);
        static GLenum getFormat(PixelFormat format);
        static GLenum getType(PixelFormat format);

    public:
        GLRenderer(mango::OpenGLContext& context);
        ~GLRenderer() override;

        bool appleClientStorage() const
        {
            return m_apple_client_storage;
        }

        void initialize() override;
        void resize(int width, int height) override;

        void beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend) override;
        void drawImage(const ImageDrawRequest& request) override;

        TextureHandle createTexture(int width, int height, PixelFormat format, const void* initial_data) override;
        void uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                 int x, int y, int width, int height, const void* pixels) override;
        void destroyTexture(TextureHandle handle) override;
    };

} // namespace ifap
