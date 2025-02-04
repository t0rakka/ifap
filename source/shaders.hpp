/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"
#include "texture.hpp"

namespace ifap
{

    struct Uniform
    {
        GLint location;
    };

    struct Uniform1i : Uniform
    {
        void set(GLint v0)
        {
            glUniform1i(location, v0);
        }
    };

    struct Uniform1f : Uniform
    {
        void set(GLfloat v)
        {
            glUniform1f(location, v);
        }
    };

    struct Uniform2f : Uniform
    {
        void set(GLfloat v0, GLfloat v1)
        {
            glUniform2f(location, v0, v1);
        }
    };

    struct Uniform4f : Uniform
    {
        void set(GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
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

    enum class TextureFilter
    {
        NEAREST,
        BILINEAR,
        BICUBIC
    };

    struct Shaders
    {
        ProgramBilinear program_bilinear;
        ProgramBicubic program_bicubic;
        TextureFilter texture_filter = TextureFilter::BILINEAR;

        GLuint vao;
        GLuint vertex_buffer;
        GLuint element_buffer;

        Shaders();
        ~Shaders();

        void draw(const Texture& texture, float intensity, float32x2 translate, float32x2 scale);
    };

} // namespace ifap
