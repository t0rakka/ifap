/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include "context.hpp"

#include <algorithm>
#include <string>

namespace ifap::shaders
{

    inline float32x2 imageTexScale(int width, int height, bool flip_y)
    {
        const float inv_w = 1.0f / float(std::max(1, width));
        const float inv_h = 1.0f / float(std::max(1, height));
        return float32x2(inv_w, flip_y ? -inv_h : inv_h);
    }

    namespace detail
    {
        inline constexpr const char* g_glsl_cubic = R"(
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
        )";

        inline constexpr const char* g_glsl_texture_filter = R"(
            vec4 texture_filter(sampler2D tex, vec2 uv, vec2 texscale)
            {
                texscale = abs(texscale);
                uv /= texscale;
                uv -= vec2(0.5, 0.5);

                float fx = fract(uv.x);
                float fy = fract(uv.y);
                uv.x -= fx;
                uv.y -= fy;

                vec4 cx = cubic(fx);
                vec4 cy = cubic(fy);

                vec4 c = vec4(uv.x - 0.5, uv.x + 1.5, uv.y - 0.5, uv.y + 1.5);
                vec4 s = vec4(cx.x + cx.y, cx.z + cx.w, cy.x + cy.y, cy.z + cy.w);
                vec4 offset = c + vec4(cx.y, cx.w, cy.y, cy.w) / s;

                vec4 sample0 = texture(tex, vec2(offset.x, offset.z) * texscale);
                vec4 sample1 = texture(tex, vec2(offset.y, offset.z) * texscale);
                vec4 sample2 = texture(tex, vec2(offset.x, offset.w) * texscale);
                vec4 sample3 = texture(tex, vec2(offset.y, offset.w) * texscale);

                float sx = s.x / (s.x + s.y);
                float sy = s.z / (s.z + s.w);

                return mix(mix(sample3, sample2, sx), mix(sample1, sample0, sx), sy);
            }
        )";

        inline constexpr const char* g_glsl_linear_to_srgb = R"(
            vec3 linearToSrgb(vec3 linear)
            {
                vec3 lo = linear * 12.92;
                vec3 hi = pow(max(linear, vec3(0.0031308)), vec3(1.0 / 2.4)) * 1.055 - 0.055;
                return mix(lo, hi, step(vec3(0.0031308), linear));
            }
        )";

        inline constexpr const char* g_glsl_vertex_main = R"(
            void main()
            {
                texcoord = inPosition * vec2(0.5, 0.5) + vec2(0.5);
                if (uTexScale.y < 0.0)
                {
                    texcoord.y = 1.0 - texcoord.y;
                }
                gl_Position = vec4((inPosition + vec2(uTransform.x, -uTransform.y)) * uTransform.zw, 0.0, 1.0);
            }
        )";

        inline constexpr const char* g_glsl_fragment_bilinear_main = R"(
            void main()
            {
                vec4 color = texture(uTexture, texcoord) * uScale;
                if (uOutputSrgbEncode > 0.5)
                {
                    color.rgb = linearToSrgb(color.rgb);
                }
                outColor = color;
            }
        )";

        inline constexpr const char* g_glsl_fragment_bicubic_main = R"(
            void main()
            {
                vec4 color = texture_filter(uTexture, texcoord, uTexScale) * uScale;
                if (uOutputSrgbEncode > 0.5)
                {
                    color.rgb = linearToSrgb(color.rgb);
                }
                outColor = color;
            }
        )";

    } // namespace detail

    inline std::string glVertexShader()
    {
        return std::string(R"(#version 330

            uniform vec4 uTransform = vec4(0.0, 0.0, 1.0, 1.0);
            uniform vec2 uTexScale = vec2(1.0, 1.0);
            in vec2 inPosition;

            out vec2 texcoord;
        )") + detail::g_glsl_vertex_main;
    }

    inline std::string glFragmentShaderBilinear()
    {
        return std::string(R"(#version 330

            uniform sampler2D uTexture;
            uniform float uScale;
            uniform float uOutputSrgbEncode = 0.0;
            in vec2 texcoord;

            out vec4 outColor;
        )") + detail::g_glsl_linear_to_srgb + detail::g_glsl_fragment_bilinear_main;
    }

    inline std::string glFragmentShaderBicubic()
    {
        return std::string(R"(#version 330

            uniform sampler2D uTexture;
            uniform vec2 uTexScale;
            uniform float uScale;
            uniform float uOutputSrgbEncode = 0.0;
            in vec2 texcoord;

            out vec4 outColor;
        )") + detail::g_glsl_cubic + detail::g_glsl_texture_filter + detail::g_glsl_linear_to_srgb + detail::g_glsl_fragment_bicubic_main;
    }

    inline std::string vkVertexShader()
    {
        return std::string(R"(#version 450
            layout(location = 0) in vec2 inPosition;
            layout(location = 0) out vec2 texcoord;

            layout(push_constant) uniform Push
            {
                vec4 uTransform;
                float uScale;
                vec2 uTexScale;
                float uOutputSrgbEncode;
            } pc;

            #define uTransform pc.uTransform
            #define uTexScale pc.uTexScale
        )") + detail::g_glsl_vertex_main;
    }

    inline std::string vkFragmentShaderBilinear()
    {
        return std::string(R"(#version 450
            layout(set = 0, binding = 0) uniform sampler2D uTexture;
            layout(location = 0) in vec2 texcoord;
            layout(location = 0) out vec4 outColor;

            layout(push_constant) uniform Push
            {
                vec4 uTransform;
                float uScale;
                vec2 uTexScale;
                float uOutputSrgbEncode;
            } pc;

            #define uScale pc.uScale
            #define uOutputSrgbEncode pc.uOutputSrgbEncode
        )") + detail::g_glsl_linear_to_srgb + detail::g_glsl_fragment_bilinear_main;
    }

    inline std::string vkFragmentShaderBicubic()
    {
        return std::string(R"(#version 450
            layout(set = 0, binding = 0) uniform sampler2D uTexture;
            layout(location = 0) in vec2 texcoord;
            layout(location = 0) out vec4 outColor;

            layout(push_constant) uniform Push
            {
                vec4 uTransform;
                float uScale;
                vec2 uTexScale;
                float uOutputSrgbEncode;
            } pc;

            #define uScale pc.uScale
            #define uTexScale pc.uTexScale
            #define uOutputSrgbEncode pc.uOutputSrgbEncode
        )") + detail::g_glsl_cubic + detail::g_glsl_texture_filter + detail::g_glsl_linear_to_srgb + detail::g_glsl_fragment_bicubic_main;
    }

} // namespace ifap::shaders
