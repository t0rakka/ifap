/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "shaders.hpp"

namespace ifap
{
    using namespace mango;

    // -----------------------------------------------------------------------
    // Shaders
    // -----------------------------------------------------------------------

    Shaders::Shaders()
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

        // create bilinear

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

        program_bilinear.program = opengl::createProgram(vertex_shader_source, fragment_shader_source);

        program_bilinear.uniform_transform.location = program_bilinear.getUniformLocation("uTransform");
        program_bilinear.uniform_texture.location = program_bilinear.getUniformLocation("uTexture");
        program_bilinear.uniform_scale.location = program_bilinear.getUniformLocation("uScale");
        program_bilinear.attribute_position = program_bilinear.getAttribLocation("inPosition");

        // create bicubic

        static const char* vertex_shader_source_bicubic = R"(
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
                // hack to bring unit texcoords to integer pixel coords
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

        program_bicubic.program = opengl::createProgram(vertex_shader_source_bicubic,
                                                        fragment_shader_source_bicubic);

        program_bicubic.uniform_transform.location = program_bicubic.getUniformLocation("uTransform");
        program_bicubic.uniform_texture.location = program_bicubic.getUniformLocation("uTexture");
        program_bicubic.uniform_texture_scale.location = program_bicubic.getUniformLocation("uTexScale");
        program_bicubic.uniform_scale.location = program_bicubic.getUniformLocation("uScale");
        program_bicubic.attribute_position = program_bicubic.getAttribLocation("inPosition");

        // vertex array objects

        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        auto createBuffer = [] (GLenum target, const void* data, GLsizei size)
        {
            GLuint buffer;
            glGenBuffers(1, &buffer);
            glBindBuffer(target, buffer);
            glBufferData(target, size, data, GL_STATIC_DRAW);
            return buffer;
        };

        vertex_buffer = createBuffer(GL_ARRAY_BUFFER, vertex_buffer_data, sizeof(vertex_buffer_data));
        element_buffer = createBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer_data, sizeof(element_buffer_data));
    }

    Shaders::~Shaders()
    {
        glDeleteProgram(program_bilinear.program);
        glDeleteProgram(program_bicubic.program);
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vertex_buffer);
        glDeleteBuffers(1, &element_buffer);
    }

    void Shaders::draw(const Texture& texture, float intensity, float32x2 translate, float32x2 scale)
    {
        glBindVertexArray(vao);

        float32x2 size;
        size.x = float(std::max(1, texture.width));
        size.y = float(std::max(1, texture.height));

        switch (texture_filter)
        {
            case TextureFilter::NEAREST:
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

                glUseProgram(program_bilinear.program);
                program_bilinear.uniform_transform.set(translate.x, -translate.y, scale.x, scale.y);
                program_bilinear.uniform_texture.set(0);
                program_bilinear.uniform_scale.set(intensity);

                glVertexAttribPointer(program_bilinear.attribute_position, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void*)0);
                glEnableVertexAttribArray(program_bilinear.attribute_position);
                break;

            case TextureFilter::BILINEAR:
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                glUseProgram(program_bilinear.program);
                program_bilinear.uniform_transform.set(translate.x, -translate.y, scale.x, scale.y);
                program_bilinear.uniform_texture.set(0);
                program_bilinear.uniform_scale.set(intensity);

                glVertexAttribPointer(program_bilinear.attribute_position, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void*)0);
                glEnableVertexAttribArray(program_bilinear.attribute_position);
                break;

            case TextureFilter::BICUBIC:
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                glUseProgram(program_bicubic.program);
                program_bicubic.uniform_transform.set(translate.x, -translate.y, scale.x, scale.y);
                program_bicubic.uniform_texture.set(0);
                program_bicubic.uniform_texture_scale.set(1.0f / size.x, 1.0f / size.y);
                program_bicubic.uniform_scale.set(intensity);

                glVertexAttribPointer(program_bicubic.attribute_position, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat) * 2, (void*)0);
                glEnableVertexAttribArray(program_bicubic.attribute_position);
                break;
        }

        glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(0));

    }

} // namespace ifap
