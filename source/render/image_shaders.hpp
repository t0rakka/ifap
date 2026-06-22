/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#pragma once

#include <string>

namespace ifap::shaders
{

    namespace detail
    {
        inline constexpr const char* g_cubic = R"(
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

        inline constexpr const char* g_texture_filter = R"(
            vec4 texture_filter(sampler2D tex, vec2 uv, vec2 texscale)
            {
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

        inline constexpr const char* g_linear_to_srgb = R"(
            vec3 linearToSrgb(vec3 linear)
            {
                vec3 lo = linear * 12.92;
                vec3 hi = pow(max(linear, vec3(0.0031308)), vec3(1.0 / 2.4)) * 1.055 - 0.055;
                return mix(lo, hi, step(vec3(0.0031308), linear));
            }
        )";

        inline constexpr const char* g_vertex_main = R"(
            void main()
            {
                texcoord = inPosition * vec2(0.5, 0.5) + vec2(0.5);
                gl_Position = vec4((inPosition + uTransform.xy) * uTransform.zw, 0.0, 1.0);
            }
        )";

        inline constexpr const char* g_fragment_bilinear_main = R"(
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

        inline constexpr const char* g_fragment_bicubic_main = R"(
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

        inline constexpr const char* g_push_constants = R"(
            layout(push_constant) uniform Push
            {
                layout(offset = 0) vec4 uTransform;
                layout(offset = 16) float uScale;
                layout(offset = 24) vec2 uTexScale;
                layout(offset = 32) float uOutputSrgbEncode;
            } pc;
        )";

    } // namespace detail

    inline std::string vertexShader()
    {
        return std::string(R"(#version 450
            layout(location = 0) in vec2 inPosition;
            layout(location = 0) out vec2 texcoord;
        )") + detail::g_push_constants + R"(
            #define uTransform pc.uTransform
        )" + detail::g_vertex_main;
    }

    inline std::string fragmentShaderBilinear()
    {
        return std::string(R"(#version 450
            layout(set = 0, binding = 0) uniform sampler2D uTexture;
            layout(location = 0) in vec2 texcoord;
            layout(location = 0) out vec4 outColor;
        )") + detail::g_push_constants + R"(
            #define uScale pc.uScale
            #define uOutputSrgbEncode pc.uOutputSrgbEncode
        )" + detail::g_linear_to_srgb + detail::g_fragment_bilinear_main;
    }

    inline std::string fragmentShaderBicubic()
    {
        return std::string(R"(#version 450
            layout(set = 0, binding = 0) uniform sampler2D uTexture;
            layout(location = 0) in vec2 texcoord;
            layout(location = 0) out vec4 outColor;
        )") + detail::g_push_constants + R"(
            #define uScale pc.uScale
            #define uTexScale pc.uTexScale
            #define uOutputSrgbEncode pc.uOutputSrgbEncode
        )" + detail::g_cubic + detail::g_texture_filter + detail::g_linear_to_srgb + detail::g_fragment_bicubic_main;
    }

} // namespace ifap::shaders
