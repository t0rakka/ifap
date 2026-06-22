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

        inline constexpr const char* g_srgb_to_linear = R"(
            vec3 srgbToLinear(vec3 encoded)
            {
                vec3 lo = encoded / 12.92;
                vec3 hi = pow(max((encoded + 0.055) / 1.055, vec3(0.0)), vec3(2.4));
                return mix(lo, hi, step(vec3(0.04045), encoded));
            }
        )";

        // SMPTE ST 2084 (PQ). Input is linear 0-1 where 1.0 = 10,000 nits.
        inline constexpr const char* g_linear_to_pq = R"(
            vec3 linearToPQ(vec3 linear)
            {
                const float m1 = 0.1593017578125;
                const float m2 = 78.84375;
                const float c1 = 0.8359375;
                const float c2 = 18.8515625;
                const float c3 = 18.6875;

                vec3 Lm = pow(max(linear, vec3(0.0)), vec3(m1));
                vec3 N = (c1 + c2 * Lm) / (1.0 + c3 * Lm);
                return pow(N, vec3(m2));
            }
        )";

        // ARIB STD-B67 / BT.2100 HLG OETF (scene-referred linear in, signal out)
        inline constexpr const char* g_linear_to_hlg = R"(
            vec3 linearToHLG(vec3 linear)
            {
                const float a = 0.17883277;
                const float b = 0.28466892;
                const float c = 0.55991073;
                vec3 x = max(linear, vec3(0.0));
                vec3 lo = sqrt(3.0 * x);
                vec3 hi = a * log(max(12.0 * x - b, 1e-6)) + c;
                return mix(lo, hi, step(vec3(1.0 / 12.0), x));
            }
        )";

        // 0 = pass, 1 = sRGB, 2 = PQ, 3 = HLG, 4 = Adobe, 5 = BT.709, 6 = DCI P3,
        // 7 = ext-sRGB linear+sRGB RT, 8 = linear surface, 9 = Display P3 linear
        // ITU-R BT.1886 reference (normalized): signal = L^(1/2.4)
        inline constexpr const char* g_linear_to_bt1886 = R"(
            vec3 linearToBt1886(vec3 linear)
            {
                return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.4));
            }
        )";

        inline constexpr const char* g_linear_to_gamma22 = R"(
            vec3 linearToGamma22(vec3 linear)
            {
                return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.2));
            }
        )";

        inline constexpr const char* g_bt709_to_bt2020 = R"(
            vec3 bt709ToBt2020(vec3 linear)
            {
                // BT.2087 BT.709 -> BT.2020; mat3 columns = input channel coefficients
                const mat3 m = mat3(
                    vec3(0.6274040, 0.0690970, 0.0163916),
                    vec3(0.3292820, 0.9195400, 0.0880132),
                    vec3(0.0433136, 0.0113612, 0.8955950)
                );
                return m * linear;
            }
        )";

        // Adobe RGB nonlinear: keep BT.709 linear (macOS ColorSync maps tagged surface
        // to the panel). Do NOT apply bt709ToAdobeRgb here — the panel is typically
        // Display P3, and a manual matrix + system conversion shifts red toward yellow.
        inline constexpr const char* g_bt709_to_adobe_rgb = R"(
            vec3 bt709ToAdobeRgb(vec3 linear)
            {
                return linear;
            }
        )";

        inline constexpr const char* g_encode_output = R"(
            vec4 encodeOutput(vec4 color)
            {
                color.rgb = max(color.rgb, vec3(0.0));

                if (uOutputTransform > 9.5)
                {
                    const float desat = 0.86;
                    vec3 linear = bt709ToBt2020(color.rgb) * (uSdrWhiteNits / 100.0);
                    float luma = dot(linear, vec3(0.2627, 0.6780, 0.0593));
                    color.rgb = mix(vec3(luma), linear, desat);
                }
                else if (uOutputTransform > 8.5)
                {
                    const float desat = 0.87;
                    vec3 linear = color.rgb * (uSdrWhiteNits / 100.0);
                    float luma = dot(linear, vec3(0.2126, 0.7152, 0.0722));
                    color.rgb = mix(vec3(luma), linear, desat);
                }
                else if (uOutputTransform > 7.5)
                {
                    color.rgb = color.rgb * (uSdrWhiteNits / 100.0);
                }
                else if (uOutputTransform > 6.5)
                {
                    vec3 linear = color.rgb * (uSdrWhiteNits / 100.0);
                    color.rgb = srgbToLinear(linear);
                }
                else if (uOutputTransform > 5.5)
                {
                    const float toe = 0.015;
                    const float desat = 0.88;
                    vec3 linear = color.rgb * (1.0 - toe) + toe;
                    float luma = dot(linear, vec3(0.2126, 0.7152, 0.0722));
                    linear = mix(vec3(luma), linear, desat);
                    color.rgb = linearToSrgb(linear) * (uSdrWhiteNits / 100.0);
                }
                else if (uOutputTransform > 4.5)
                {
                    const float toe = 0.02;
                    const float contrast = 1.05;
                    vec3 linear = color.rgb * (1.0 - toe) + toe;
                    linear = clamp((linear - 0.5) * contrast + 0.5, vec3(0.0), vec3(1.0));
                    color.rgb = linearToSrgb(linear) * (uSdrWhiteNits / 100.0);
                }
                else if (uOutputTransform > 3.5)
                {
                    vec3 linear = bt709ToAdobeRgb(color.rgb) * (uSdrWhiteNits / 100.0);
                    float luma = dot(linear, vec3(0.2126, 0.7152, 0.0722));
                    linear = mix(vec3(luma), linear, 0.90);
                    color.rgb = linearToGamma22(linear);
                }
                else if (uOutputTransform > 2.5)
                {
                    vec3 linear = bt709ToBt2020(color.rgb);
                    // uSdrWhiteNits is a 0..100 scale knob here, not literal nits (see PQ branch).
                    color.rgb = linearToHLG(linear * (uSdrWhiteNits / 100.0));
                }
                else if (uOutputTransform > 1.5)
                {
                    const float peakNits = 10000.0;
                    vec3 linear = bt709ToBt2020(color.rgb);
                    color.rgb = linearToPQ(linear * (uSdrWhiteNits / peakNits));
                }
                else if (uOutputTransform > 0.5)
                {
                    color.rgb = linearToSrgb(color.rgb);
                }
                return color;
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
                vec4 color = encodeOutput(texture(uTexture, texcoord) * uScale);
                outColor = color;
            }
        )";

        inline constexpr const char* g_fragment_bicubic_main = R"(
            void main()
            {
                vec4 color = encodeOutput(texture_filter(uTexture, texcoord, uTexScale) * uScale);
                outColor = color;
            }
        )";

        inline constexpr const char* g_push_constants = R"(
            layout(push_constant) uniform Push
            {
                layout(offset = 0) vec4 uTransform;
                layout(offset = 16) float uScale;
                layout(offset = 24) vec2 uTexScale;
                layout(offset = 32) float uOutputTransform;
                layout(offset = 36) float uSdrWhiteNits;
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
            #define uOutputTransform pc.uOutputTransform
            #define uSdrWhiteNits pc.uSdrWhiteNits
        )" + detail::g_linear_to_srgb + detail::g_srgb_to_linear + detail::g_linear_to_bt1886 + detail::g_linear_to_gamma22 + detail::g_linear_to_pq + detail::g_linear_to_hlg + detail::g_bt709_to_bt2020 + detail::g_bt709_to_adobe_rgb + detail::g_encode_output + detail::g_fragment_bilinear_main;
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
            #define uOutputTransform pc.uOutputTransform
            #define uSdrWhiteNits pc.uSdrWhiteNits
        )" + detail::g_cubic + detail::g_texture_filter + detail::g_linear_to_srgb + detail::g_srgb_to_linear + detail::g_linear_to_bt1886 + detail::g_linear_to_gamma22 + detail::g_linear_to_pq + detail::g_linear_to_hlg + detail::g_bt709_to_bt2020 + detail::g_bt709_to_adobe_rgb + detail::g_encode_output + detail::g_fragment_bicubic_main;
    }

} // namespace ifap::shaders
