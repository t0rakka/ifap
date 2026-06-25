/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "vk_renderer.hpp"
#include "../image_shaders.hpp"

#include <algorithm>
#include <cstring>
#include <cstddef>
#include <vector>

#include <mango/vulkan/vulkan.hpp>
#include <mango/vulkan/compiler.hpp>

namespace ifap
{
    using namespace mango;
    using namespace mango::vulkan;

    namespace
    {
        struct ProcessingPushConstants
        {
            float transform[4];
            float texScale[2];
        };

        struct ResolvePushConstants
        {
            float outputTransform;
            float sdrWhiteNits;
        };

        static_assert(offsetof(ProcessingPushConstants, texScale) == 16);
        static_assert(sizeof(ProcessingPushConstants) == 24);
        static_assert(sizeof(ResolvePushConstants) == 8);

        constexpr VkFormat kProcessingFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

        enum class OutputTransform : int
        {
            Pass = 0,           // sRGB surface: texture decode is sufficient
            SrgbEncode = 1,       // float/UNORM SDR: linear BT.709 -> sRGB gamma
            SdrToHdrPQ = 2,       // HDR10 ST2084: BT.709 -> BT.2020 -> PQ
            SdrToHdrHLG = 3,      // HDR10 HLG: BT.709 -> BT.2020 -> BT.2408 gain -> HLG OETF
            SdrToAdobeRgb = 4,    // Adobe RGB nonlinear: BT.709 -> Adobe RGB -> Adobe OETF
            SdrToBt709Nonlinear = 5, // BT.709 nonlinear: SMPTE 170M / ITU OETF
            SdrToDciP3Nonlinear = 6, // DCI-P3 nonlinear: BT.709 -> P3 -> gamma 2.6
            SdrToExtendedSrgbLinear = 7, // EXTENDED_SRGB_LINEAR + sRGB RT: pre-compensate encode
            SdrToLinearSurface = 8,      // linear color space + UNORM/float: linear passthrough
            SdrToDisplayP3Linear = 9,    // Display P3 linear float: mild desat + linear out
            SdrToBt2020Linear = 10,      // BT.2020 linear float: desat + linear out
        };

        static bool isSrgbSurfaceFormat(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_SRGB:
                case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
                    return true;
                default:
                    return false;
            }
        }

        static bool isLinearSdrColorSpace(VkColorSpaceKHR colorSpace)
        {
            switch (colorSpace)
            {
                case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
                case VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT:
                case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
                    return true;
                default:
                    return false;
            }
        }

        static bool isHdrColorSpace(VkColorSpaceKHR colorSpace)
        {
            switch (colorSpace)
            {
                case VK_COLOR_SPACE_HDR10_ST2084_EXT:
                case VK_COLOR_SPACE_HDR10_HLG_EXT:
                case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
                    return true;
                default:
                    return false;
            }
        }

        static bool surfaceFormatMatches(const VkSurfaceFormatKHR& a, const VkSurfaceFormatKHR& b)
        {
            return a.format == b.format && a.colorSpace == b.colorSpace;
        }

        static bool containsSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats, const VkSurfaceFormatKHR& candidate)
        {
            for (const VkSurfaceFormatKHR& format : formats)
            {
                if (surfaceFormatMatches(format, candidate))
                {
                    return true;
                }
            }

            return false;
        }

        static OutputTransform selectOutputTransform(const VkSurfaceFormatKHR& surfaceFormat)
        {
            if (surfaceFormat.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
            {
                return OutputTransform::SdrToHdrPQ;
            }

            if (surfaceFormat.colorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT)
            {
                return OutputTransform::SdrToHdrHLG;
            }

            if (surfaceFormat.colorSpace == VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT)
            {
                return OutputTransform::SdrToAdobeRgb;
            }

            if (surfaceFormat.colorSpace == VK_COLOR_SPACE_BT709_NONLINEAR_EXT)
            {
                return OutputTransform::SdrToBt709Nonlinear;
            }

            if (surfaceFormat.colorSpace == VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT)
            {
                return OutputTransform::SdrToDciP3Nonlinear;
            }

            if (isLinearSdrColorSpace(surfaceFormat.colorSpace))
            {
                if (surfaceFormat.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
                    && isSrgbSurfaceFormat(surfaceFormat.format))
                {
                    return OutputTransform::SdrToExtendedSrgbLinear;
                }

                if (surfaceFormat.colorSpace == VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT)
                {
                    return OutputTransform::SdrToDisplayP3Linear;
                }

                if (surfaceFormat.colorSpace == VK_COLOR_SPACE_BT2020_LINEAR_EXT)
                {
                    return OutputTransform::SdrToBt2020Linear;
                }

                return OutputTransform::SdrToLinearSurface;
            }

            if (!isSrgbSurfaceFormat(surfaceFormat.format))
            {
                return OutputTransform::SrgbEncode;
            }

            return OutputTransform::Pass;
        }

        // uSdrWhiteNits: SDR diffuse white in nits.
        //   PQ shader:   linear * nits / 10000
        //   HLG shader:  BT.2408 gain 0.265 * (nits / 203) before OETF
        static float defaultSdrWhiteNits(VkColorSpaceKHR colorSpace)
        {
            switch (colorSpace)
            {
                case VK_COLOR_SPACE_HDR10_ST2084_EXT:
                case VK_COLOR_SPACE_HDR10_HLG_EXT:
                    return 100.0f;
                default:
                    return 100.0f;
            }
        }

        // ------------------------------------------------------------------
        // Resolve pass: scene-linear BT.709 in -> swapchain colorspace out.
        // ------------------------------------------------------------------
        //
        // >>> ACTIVE TARGET: the single uncommented kDevSurfaceFormat line below.
        //     Read it before tuning — defaults/calibration apply to THAT color space only.
        //
        // VkFormat        — how samples are stored (layout, UNORM/SFLOAT/SRGB, …)
        // VkColorSpaceKHR — what those values mean at scanout (primaries + EOTF)
        //
        // SDR baseline goal: same sRGB JPEG should look correct per tagged colorspace.
        // Texture side: Mango decoder sets header.linear → RGBA8_SRGB vs UNORM.
        //
        // HLG:      BT.709 -> BT.2020, x0.265*(nits/203) (BT.2408), HLG OETF
        // Adobe:    BT.709 -> Adobe RGB (XYZ-derived matrix), OETF exponent 256/563
        // BT709:    SMPTE 170M / ITU piecewise OETF (same primaries)
        // DCI-P3:   BT.709 -> P3 primaries, gamma 2.6
        // ------------------------------------------------------------------

        // KNOWN ISSUE (Hyprland/Wayland, June 2026): an HDR / wide-gamut *windowed*
        // swapchain flickers on resize (cleared frame shown before content). This is a
        // compositor-side limitation, not an ifap rendering bug. HDR-first selection
        // works great on Windows 11 and macOS/MoltenVK; only Hyprland/Wayland misbehaves.
        //
        // Diagnosis: vkbasic with an SDR swapchain does not flicker, and forcing ifap to
        // the same SDR format (B8G8R8A8_UNORM / SRGB_NONLINEAR) makes resize smooth in
        // both OnDemand and Continuous modes. Frame mode is NOT the cause.
        //
        // Wayland quirk: in windowed mode the HDR10 colorspace is effectively SDR anyway
        // (the compositor tonemaps HDR10 -> SDR display), so we lose nothing by shipping
        // HDR-first here. Going fullscreen flips the display into real HDR (gorgeous), but
        // exiting fullscreen leaves it stuck in HDR — hyprctl can't recover it; only a
        // logout does. Their HDR pipeline is simply not there yet.
        //
        // Decision: keep HDR-first auto ON by default (kDevForceSurfaceFormat = false) so
        // ifap is ready to "just work" once a Wayland compositor ships a correct HDR impl.
        // Set this to true (with the SDR entry below) to force SDR for flicker-free
        // testing. Intended long-term mitigation: fullscreen-gated HDR (SDR windowed,
        // HDR-first in fullscreen).
        constexpr bool kDevForceSurfaceFormat = false;

        constexpr VkSurfaceFormatKHR kDevSurfaceFormat =
        {
            // OK
            //VK_FORMAT_B8G8R8A8_UNORM,      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            //VK_FORMAT_B8G8R8A8_SRGB,       VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_PASS_THROUGH_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_ST2084_EXT
            VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT
            //VK_FORMAT_B8G8R8A8_SRGB,       VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
            //VK_FORMAT_B8G8R8A8_UNORM,      VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_BT2020_LINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_HLG_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_BT709_NONLINEAR_EXT
            //VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT
        };

        static VkSurfaceFormatKHR selectBestFormatForColorSpace(
            const std::vector<VkSurfaceFormatKHR>& surfaceFormats,
            VkColorSpaceKHR colorSpace)
        {
            static const VkFormat formatPreference[] =
            {
                VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_UNORM,
            };

            for (VkFormat format : formatPreference)
            {
                const VkSurfaceFormatKHR candidate { format, colorSpace };

                if (containsSurfaceFormat(surfaceFormats, candidate))
                {
                    return candidate;
                }
            }

            return surfaceFormats.empty()
                ? VkSurfaceFormatKHR { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
                : surfaceFormats[0];
        }

        static VkSurfaceFormatKHR selectSurfaceFormat(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
        {
            // HDR-first auto selection; per-color-space output calibration comes later.
            static const VkSurfaceFormatKHR preferredFormats[] =
            {
                { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_ST2084_EXT },
                { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },
                { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },

                { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_HLG_EXT },
                { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_HLG_EXT },

                { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_BT2020_LINEAR_EXT },
                { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_BT2020_LINEAR_EXT },

                { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT },
                { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT },
                { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT },

                { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
                { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
            };

            const std::vector<VkSurfaceFormatKHR> surfaceFormats = getSurfaceFormats(physicalDevice, surface);

            VkSurfaceFormatKHR selectedFormat = preferredFormats[std::size(preferredFormats) - 1];
            bool forced = false;

            if (kDevForceSurfaceFormat)
            {
                if (containsSurfaceFormat(surfaceFormats, kDevSurfaceFormat))
                {
                    selectedFormat = kDevSurfaceFormat;
                    forced = true;
                }
                else if (containsSurfaceFormat(surfaceFormats,
                    selectBestFormatForColorSpace(surfaceFormats, kDevSurfaceFormat.colorSpace)))
                {
                    selectedFormat = selectBestFormatForColorSpace(surfaceFormats, kDevSurfaceFormat.colorSpace);
                    forced = true;
                    printLine(Print::Warning, "VKRenderer: exact dev format unavailable; using best format for {}",
                        getString(kDevSurfaceFormat.colorSpace));
                }
                else
                {
                    printLine(Print::Warning, "VKRenderer: dev color space {} not supported; using auto selection",
                        getString(kDevSurfaceFormat.colorSpace));
                }
            }

            if (!forced && !surfaceFormats.empty())
            {
                selectedFormat = surfaceFormats[0];

                for (const VkSurfaceFormatKHR& preferred : preferredFormats)
                {
                    if (containsSurfaceFormat(surfaceFormats, preferred))
                    {
                        selectedFormat = preferred;
                        break;
                    }
                }
            }

            printLine(Print::Info, "VKRenderer: PhysicalDeviceSurfaceFormats:");

            for (const VkSurfaceFormatKHR& format : surfaceFormats)
            {
                const bool is_selected = surfaceFormatMatches(format, selectedFormat);
                printLine(Print::Info, "  {} {} | {}",
                    is_selected ? ">" : " ",
                    getString(format.format),
                    getString(format.colorSpace));
            }

            if (forced)
            {
                printLine(Print::Info, "VKRenderer: using dev-forced surface format");
            }

            return selectedFormat;
        }

        VkPipelineColorBlendAttachmentState makeBlendAttachment(bool blend)
        {
            VkPipelineColorBlendAttachmentState state =
            {
                .blendEnable = blend ? VK_TRUE : VK_FALSE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
            };

            return state;
        }

        VkPipeline createGraphicsPipeline(VkDevice device, VkFormat colorFormat, VkPipelineLayout layout,
                                          VkShaderModule vertexShader, VkShaderModule fragmentShader,
                                          bool blend)
        {
            VkPipelineShaderStageCreateInfo stages[] =
            {
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_VERTEX_BIT,
                    .module = vertexShader,
                    .pName = "main",
                },
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                    .module = fragmentShader,
                    .pName = "main",
                },
            };

            VkVertexInputBindingDescription binding =
            {
                .binding = 0,
                .stride = sizeof(float) * 2,
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };

            VkVertexInputAttributeDescription attribute =
            {
                .location = 0,
                .binding = 0,
                .format = VK_FORMAT_R32G32_SFLOAT,
                .offset = 0,
            };

            VkPipelineVertexInputStateCreateInfo vertexInput =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                .vertexBindingDescriptionCount = 1,
                .pVertexBindingDescriptions = &binding,
                .vertexAttributeDescriptionCount = 1,
                .pVertexAttributeDescriptions = &attribute,
            };

            VkPipelineInputAssemblyStateCreateInfo inputAssembly =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            };

            // Viewport and scissor are dynamic so a window resize never triggers a
            // pipeline rebuild; the live extent is set per command buffer instead.
            VkPipelineViewportStateCreateInfo viewportState =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .viewportCount = 1,
                .scissorCount = 1,
            };

            VkDynamicState dynamicStates[] =
            {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR,
            };

            VkPipelineDynamicStateCreateInfo dynamicState =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                .dynamicStateCount = 2,
                .pDynamicStates = dynamicStates,
            };

            VkPipelineRasterizationStateCreateInfo rasterizer =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .polygonMode = VK_POLYGON_MODE_FILL,
                .cullMode = VK_CULL_MODE_NONE,
                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                .lineWidth = 1.0f,
            };

            VkPipelineMultisampleStateCreateInfo multisampling =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            };

            VkPipelineColorBlendAttachmentState blendAttachment = makeBlendAttachment(blend);

            VkPipelineColorBlendStateCreateInfo colorBlending =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments = &blendAttachment,
            };

            VkPipelineRenderingCreateInfo renderingCreateInfo =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .colorAttachmentCount = 1,
                .pColorAttachmentFormats = &colorFormat,
            };

            VkGraphicsPipelineCreateInfo pipelineInfo =
            {
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .pNext = &renderingCreateInfo,
                .stageCount = 2,
                .pStages = stages,
                .pVertexInputState = &vertexInput,
                .pInputAssemblyState = &inputAssembly,
                .pViewportState = &viewportState,
                .pRasterizationState = &rasterizer,
                .pMultisampleState = &multisampling,
                .pColorBlendState = &colorBlending,
                .pDynamicState = &dynamicState,
                .layout = layout,
            };

            VkPipeline pipeline = VK_NULL_HANDLE;
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
            return pipeline;
        }

    } // namespace

    struct VKRenderer::Impl
    {
        VulkanWindow& m_window;

        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        uint32_t m_graphicsQueueFamilyIndex = 0;
        VkCommandPool m_graphicsCommandPool = VK_NULL_HANDLE;
        VkCommandPool m_transferCommandPool = VK_NULL_HANDLE;
        std::unique_ptr<Swapchain> m_swapchain;
        std::vector<VkCommandBuffer> m_commandBuffers;
        VkShaderModule m_processingVertexShader = VK_NULL_HANDLE;
        VkShaderModule m_processingFragmentShaderBilinear = VK_NULL_HANDLE;
        VkShaderModule m_processingFragmentShaderBicubic = VK_NULL_HANDLE;
        VkShaderModule m_resolveVertexShader = VK_NULL_HANDLE;
        VkShaderModule m_resolveFragmentShader = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_contentDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_resolveDescriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_processingPipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_resolvePipelineLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet m_resolveDescriptor = VK_NULL_HANDLE;
        VkPipeline m_pipelineBilinearBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBilinearNoBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBicubicBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBicubicNoBlend = VK_NULL_HANDLE;
        VkPipeline m_resolvePipeline = VK_NULL_HANDLE;
        VkImage m_processingImage = VK_NULL_HANDLE;
        VkDeviceMemory m_processingMemory = VK_NULL_HANDLE;
        VkImageView m_processingView = VK_NULL_HANDLE;
        VkImageLayout m_processingLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExtent2D m_processingExtent { 0, 0 };
        VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
        VkSampler m_samplerNearest = VK_NULL_HANDLE;
        VkSampler m_samplerLinear = VK_NULL_HANDLE;

        static constexpr int kUploadSlotCount = 2;

        struct UploadSlot
        {
            VkFence fence = VK_NULL_HANDLE;
            VkCommandBuffer command_buffer = VK_NULL_HANDLE;
            // Persistent, persistently-mapped staging buffer reused across uploads.
            // Avoids a vkAllocateMemory per region per frame (the OpenGL backend let
            // the driver manage staging; doing it by hand here was a regression).
            VkBuffer staging_buffer = VK_NULL_HANDLE;
            VkDeviceMemory staging_memory = VK_NULL_HANDLE;
            VkDeviceSize staging_capacity = 0;
            void* staging_mapped = nullptr;
            bool pending = false;
        };

        struct GpuTexture
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet descriptor = VK_NULL_HANDLE;
            UploadSlot upload_slots[kUploadSlotCount] = {};
            PixelFormat format = PixelFormat::RGBA8_UNORM;
            int width = 0;
            int height = 0;
            bool layout_ready = false;
        };

        std::vector<std::unique_ptr<GpuTexture>> m_textures;
        Swapchain::Frame m_frame;
        bool m_frame_active = false;
        bool m_command_buffer_recording = false;
        bool m_rendering_active = false;
        bool m_processing_rendering_active = false;
        bool m_content_drawn = false;
        bool m_blend_enabled = true;
        bool m_swapchain_srgb_format = false;
        OutputTransform m_output_transform = OutputTransform::Pass;
        float m_sdr_white_nits = 100.0f;
        int m_max_texture_dimension = 0;
        float m_clear_color[4] = { 0.06f, 0.06f, 0.06f, 1.0f };

        void createDevice(VkInstance instance);
        void createPipelines();
        void destroyPipelines();
        void createShaders();
        void createGeometry();
        void createSamplers();
        void createDescriptorResources();
        void createProcessingTarget();
        void destroyProcessingTarget();
        void ensureProcessingTarget();
        void createCommandBuffers();
        void beginCommandBufferRecording();
        void beginProcessingRendering();
        void endProcessingRendering();
        void setDynamicViewportScissor(VkCommandBuffer commandBuffer) const;
        void recordResolve();
        GpuTexture* getTexture(TextureHandle handle);
        const GpuTexture* getTexture(TextureHandle handle) const;
        static VkFormat toVkFormat(PixelFormat format);
        static size_t bytesPerPixel(PixelFormat format);
        uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
        void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                          VkBuffer& buffer, VkDeviceMemory& memory) const;
        void createImage(int width, int height, VkFormat format, VkImageUsageFlags usage,
                         VkImage& image, VkDeviceMemory& memory) const;
        void createImageView(VkImage image, VkFormat format, VkImageView& view) const;
        void releaseUploadSlot(UploadSlot& slot);
        void destroyUploadSlotStaging(UploadSlot& slot);
        void ensureStagingCapacity(UploadSlot& slot, VkDeviceSize size);
        void recycleUploadSlots(GpuTexture& texture, bool wait);
        UploadSlot* acquireUploadSlot(GpuTexture& texture);
        size_t submitUploadRegions(GpuTexture& texture, const TextureRegionUpload* regions, size_t count);
        VkSampler selectSampler(TextureFilter filter) const;
        VkPipeline selectPipeline(const ImageDrawRequest& request) const;
        void recordDraw(const ImageDrawRequest& request);

        Impl(VulkanWindow& window, Instance& instance, VkSurfaceKHR surface);
        ~Impl();

        void initialize();
        void resize(int width, int height);
        bool beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend);
        void beginSwapchainRendering();
        void endSwapchainRendering();
        void drawImage(const ImageDrawRequest& request);
        void endFrame();
        TextureHandle createTexture(int width, int height, PixelFormat format, const void* initial_data);
        void uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                 int x, int y, int width, int height, const void* pixels);
        size_t uploadTextureRegions(TextureHandle handle, PixelFormat format,
                                    const TextureRegionUpload* regions, size_t count);
        void destroyTexture(TextureHandle handle);
        bool tryDestroyTexture(TextureHandle handle);
        void freeTextureResources(GpuTexture& texture, TextureHandle handle);
        int getMaxTextureDimension() const;
    };

    VKRenderer::Impl::Impl(VulkanWindow& window, Instance& instance, VkSurfaceKHR surface)
        : m_window(window)
    {
        createDevice(instance);

        const VkSurfaceFormatKHR selectedFormat = selectSurfaceFormat(m_physicalDevice, surface);

        m_swapchain_srgb_format = isSrgbSurfaceFormat(selectedFormat.format);
        m_output_transform = selectOutputTransform(selectedFormat);
        m_sdr_white_nits = defaultSdrWhiteNits(selectedFormat.colorSpace);

        printLine(Print::Info, "VKRenderer: selected {} | {} (hardware sRGB: {}, output transform: {}, HDR: {}, SDR white: {} nits)",
            getString(selectedFormat.format),
            getString(selectedFormat.colorSpace),
            m_swapchain_srgb_format ? "yes" : "no",
            int(m_output_transform),
            isHdrColorSpace(selectedFormat.colorSpace) ? "yes" : "no",
            int(m_sdr_white_nits));

        m_swapchain = std::make_unique<Swapchain>(m_device, m_physicalDevice, surface, selectedFormat, m_graphicsQueue, &window);

        VkCommandPoolCreateInfo poolInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = m_graphicsQueueFamilyIndex,
        };

        vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_graphicsCommandPool);

        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_transferCommandPool);

        createShaders();
        createSamplers();
        createDescriptorResources();
        createGeometry();
        createPipelines();
        createCommandBuffers();
        createProcessingTarget();
    }

    VKRenderer::Impl::~Impl()
    {
        if (m_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_device);

            for (size_t i = 0; i < m_textures.size(); ++i)
            {
                if (m_textures[i])
                {
                    destroyTexture(TextureHandle(i + 1));
                }
            }
            m_textures.clear();

            m_swapchain.reset();

            destroyProcessingTarget();
            destroyPipelines();

            if (m_descriptorPool)
            {
                vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            }

            if (m_contentDescriptorSetLayout)
            {
                vkDestroyDescriptorSetLayout(m_device, m_contentDescriptorSetLayout, nullptr);
            }

            if (m_resolveDescriptorSetLayout)
            {
                vkDestroyDescriptorSetLayout(m_device, m_resolveDescriptorSetLayout, nullptr);
            }

            if (m_processingPipelineLayout)
            {
                vkDestroyPipelineLayout(m_device, m_processingPipelineLayout, nullptr);
            }

            if (m_resolvePipelineLayout)
            {
                vkDestroyPipelineLayout(m_device, m_resolvePipelineLayout, nullptr);
            }

            if (m_processingVertexShader)
            {
                vkDestroyShaderModule(m_device, m_processingVertexShader, nullptr);
            }

            if (m_processingFragmentShaderBilinear)
            {
                vkDestroyShaderModule(m_device, m_processingFragmentShaderBilinear, nullptr);
            }

            if (m_processingFragmentShaderBicubic)
            {
                vkDestroyShaderModule(m_device, m_processingFragmentShaderBicubic, nullptr);
            }

            if (m_resolveVertexShader)
            {
                vkDestroyShaderModule(m_device, m_resolveVertexShader, nullptr);
            }

            if (m_resolveFragmentShader)
            {
                vkDestroyShaderModule(m_device, m_resolveFragmentShader, nullptr);
            }

            if (m_vertexBuffer)
            {
                vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
            }

            if (m_vertexBufferMemory)
            {
                vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
            }

            if (m_samplerNearest)
            {
                vkDestroySampler(m_device, m_samplerNearest, nullptr);
            }

            if (m_samplerLinear)
            {
                vkDestroySampler(m_device, m_samplerLinear, nullptr);
            }

            vkDestroyCommandPool(m_device, m_transferCommandPool, nullptr);
            vkDestroyCommandPool(m_device, m_graphicsCommandPool, nullptr);
            vkDestroyDevice(m_device, nullptr);
        }
    }

    void VKRenderer::Impl::createDevice(VkInstance instance)
    {
        m_physicalDevice = selectPhysicalDevice(instance);

        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(m_physicalDevice, &deviceProperties);
        m_max_texture_dimension = int(deviceProperties.limits.maxImageDimension2D);

        std::vector<VkQueueFamilyProperties> queueFamilies = getPhysicalDeviceQueueFamilyProperties(m_physicalDevice);

        uint32_t queueFamilyIndex = UINT32_MAX;

        for (uint32_t i = 0; i < queueFamilies.size(); ++i)
        {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                getPresentationSupport(m_physicalDevice, i, m_window))
            {
                queueFamilyIndex = i;
                break;
            }
        }

        if (queueFamilyIndex == UINT32_MAX)
        {
            printLine(Print::Error, "VKRenderer: no suitable graphics queue.");
            return;
        }

        float queuePriority = 1.0f;

        VkDeviceQueueCreateInfo queueCreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queueFamilyIndex,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority,
        };

        std::vector<const char*> deviceExtensions = requiredDeviceExtensions();

        VkPhysicalDeviceVulkan13Features features13 =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .dynamicRendering = VK_TRUE,
        };

        VkDeviceCreateInfo deviceCreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features13,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueCreateInfo,
            .enabledExtensionCount = u32(deviceExtensions.size()),
            .ppEnabledExtensionNames = deviceExtensions.data(),
        };

        VkResult result = vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device);
        if (result != VK_SUCCESS)
        {
            printLine(Print::Error, "vkCreateDevice: {}", getString(result));
            return;
        }

        m_graphicsQueueFamilyIndex = queueFamilyIndex;
        vkGetDeviceQueue(m_device, m_graphicsQueueFamilyIndex, 0, &m_graphicsQueue);
    }

    void VKRenderer::Impl::initialize()
    {
    }

    int VKRenderer::Impl::getMaxTextureDimension() const
    {
        return m_max_texture_dimension;
    }

    void VKRenderer::Impl::resize(int width, int height)
    {
        MANGO_UNREFERENCED(width);
        MANGO_UNREFERENCED(height);
    }

    VkFormat VKRenderer::Impl::toVkFormat(PixelFormat format)
    {
        switch (format)
        {
            case PixelFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
            case PixelFormat::RGBA8_SRGB:  return VK_FORMAT_R8G8B8A8_SRGB;
            case PixelFormat::RGBA16F:     return VK_FORMAT_R16G16B16A16_SFLOAT;
            case PixelFormat::RGBA32F:     return VK_FORMAT_R32G32B32A32_SFLOAT;
        }

        return VK_FORMAT_R8G8B8A8_UNORM;
    }

    size_t VKRenderer::Impl::bytesPerPixel(PixelFormat format)
    {
        switch (format)
        {
            case PixelFormat::RGBA8_UNORM:
            case PixelFormat::RGBA8_SRGB:
                return 4;

            case PixelFormat::RGBA16F:
                return 8;

            case PixelFormat::RGBA32F:
                return 16;
        }

        return 4;
    }

    uint32_t VKRenderer::Impl::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }

        printLine(Print::Error, "VKRenderer: failed to find memory type.");
        return 0;
    }

    void VKRenderer::Impl::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                                  VkBuffer& buffer, VkDeviceMemory& memory) const
    {
        VkBufferCreateInfo bufferInfo =
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer);

        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(m_device, buffer, &requirements);

        VkMemoryAllocateInfo allocInfo =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties),
        };

        vkAllocateMemory(m_device, &allocInfo, nullptr, &memory);
        vkBindBufferMemory(m_device, buffer, memory, 0);
    }

    void VKRenderer::Impl::createImage(int width, int height, VkFormat format, VkImageUsageFlags usage,
                                       VkImage& image, VkDeviceMemory& memory) const
    {
        VkImageCreateInfo imageInfo =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = { u32(width), u32(height), 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        vkCreateImage(m_device, &imageInfo, nullptr, &image);

        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(m_device, image, &requirements);

        VkMemoryAllocateInfo allocInfo =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };

        vkAllocateMemory(m_device, &allocInfo, nullptr, &memory);
        vkBindImageMemory(m_device, image, memory, 0);
    }

    void VKRenderer::Impl::createImageView(VkImage image, VkFormat format, VkImageView& view) const
    {
        VkImageViewCreateInfo viewInfo =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        vkCreateImageView(m_device, &viewInfo, nullptr, &view);
    }

    VKRenderer::Impl::GpuTexture* VKRenderer::Impl::getTexture(TextureHandle handle)
    {
        if (handle == 0 || handle > m_textures.size())
        {
            return nullptr;
        }

        return m_textures[handle - 1].get();
    }

    const VKRenderer::Impl::GpuTexture* VKRenderer::Impl::getTexture(TextureHandle handle) const
    {
        if (handle == 0 || handle > m_textures.size())
        {
            return nullptr;
        }

        return m_textures[handle - 1].get();
    }

    void VKRenderer::Impl::releaseUploadSlot(UploadSlot& slot)
    {
        // Frees the transient command buffer and marks the slot idle. The persistent
        // staging buffer is intentionally kept for reuse (destroyed only with the
        // texture, in destroyUploadSlotStaging()).
        if (slot.command_buffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_device, m_transferCommandPool, 1, &slot.command_buffer);
            slot.command_buffer = VK_NULL_HANDLE;
        }

        slot.pending = false;
    }

    void VKRenderer::Impl::destroyUploadSlotStaging(UploadSlot& slot)
    {
        if (slot.staging_mapped)
        {
            vkUnmapMemory(m_device, slot.staging_memory);
            slot.staging_mapped = nullptr;
        }

        if (slot.staging_buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, slot.staging_buffer, nullptr);
            slot.staging_buffer = VK_NULL_HANDLE;
        }

        if (slot.staging_memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, slot.staging_memory, nullptr);
            slot.staging_memory = VK_NULL_HANDLE;
        }

        slot.staging_capacity = 0;
    }

    void VKRenderer::Impl::ensureStagingCapacity(UploadSlot& slot, VkDeviceSize size)
    {
        if (slot.staging_buffer != VK_NULL_HANDLE && slot.staging_capacity >= size)
        {
            return;
        }

        // Round up so repeated similar-sized batches don't reallocate (a full-image
        // slot settles at the 8 MB batch size; a 1x1 placeholder stays at 1 MB).
        static constexpr VkDeviceSize kGranularity = 1 * 1024 * 1024;
        VkDeviceSize capacity = ((size + (kGranularity - 1)) / kGranularity) * kGranularity;
        if (capacity == 0)
        {
            capacity = kGranularity;
        }

        // The caller only ever grows an idle slot (acquireUploadSlot returns slots
        // whose fence has signalled), so replacing the buffer here is safe.
        destroyUploadSlotStaging(slot);

        createBuffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     slot.staging_buffer, slot.staging_memory);

        vkMapMemory(m_device, slot.staging_memory, 0, capacity, 0, &slot.staging_mapped);
        slot.staging_capacity = capacity;
    }

    void VKRenderer::Impl::recycleUploadSlots(GpuTexture& texture, bool wait)
    {
        for (UploadSlot& slot : texture.upload_slots)
        {
            if (!slot.pending || !slot.fence)
            {
                continue;
            }

            VkResult result = wait
                ? vkWaitForFences(m_device, 1, &slot.fence, VK_TRUE, UINT64_MAX)
                : vkGetFenceStatus(m_device, slot.fence);

            if (result == VK_SUCCESS)
            {
                releaseUploadSlot(slot);
            }
            else if (wait && result != VK_ERROR_DEVICE_LOST)
            {
                printLine(Print::Error, "VKRenderer: upload fence wait failed: {}", getString(result));
            }
        }
    }

    VKRenderer::Impl::UploadSlot* VKRenderer::Impl::acquireUploadSlot(GpuTexture& texture)
    {
        recycleUploadSlots(texture, false);

        for (UploadSlot& slot : texture.upload_slots)
        {
            if (!slot.pending)
            {
                return &slot;
            }
        }

        return nullptr;
    }

    size_t VKRenderer::Impl::submitUploadRegions(GpuTexture& texture, const TextureRegionUpload* regions, size_t count)
    {
        if (!regions || count == 0)
        {
            return 0;
        }

        UploadSlot* slot = acquireUploadSlot(texture);
        if (!slot)
        {
            return 0;
        }

        static constexpr VkDeviceSize kMaxUploadBytesPerBatch = 8 * 1024 * 1024;
        // Buffer copy offsets must be a multiple of the texel block size and of 4;
        // 16 satisfies RGBA8 (4), RGBA16F (8) and RGBA32F (16).
        static constexpr VkDeviceSize kBufferOffsetAlign = 16;

        const VkDeviceSize bpp = bytesPerPixel(texture.format);

        struct BatchEntry
        {
            size_t index;
            VkDeviceSize offset;
            VkDeviceSize size;
        };

        std::vector<BatchEntry> batch;
        batch.reserve(count);

        VkDeviceSize cursor = 0;
        size_t consumed = 0;

        // Select a contiguous run of regions and pack them into one staging buffer.
        for (size_t i = 0; i < count; ++i)
        {
            const TextureRegionUpload& region = regions[i];

            if (!region.pixels || region.width <= 0 || region.height <= 0)
            {
                consumed = i + 1;
                continue;
            }

            if (region.x < 0 || region.y < 0 ||
                region.x + region.width > texture.width ||
                region.y + region.height > texture.height)
            {
                printLine(Print::Error, "VKRenderer: upload rect out of bounds ({}x{} at {},{} in {}x{})",
                    region.width, region.height, region.x, region.y, texture.width, texture.height);
                consumed = i + 1;
                continue;
            }

            const VkDeviceSize rowBytes = VkDeviceSize(region.width) * bpp;
            const VkDeviceSize imageSize = rowBytes * VkDeviceSize(region.height);
            const VkDeviceSize offset = (cursor + (kBufferOffsetAlign - 1)) & ~(kBufferOffsetAlign - 1);

            // Always accept the first region (even if it exceeds the budget); stop
            // once adding another would push the batch past the per-frame budget.
            if (!batch.empty() && offset + imageSize > kMaxUploadBytesPerBatch)
            {
                break;
            }

            batch.push_back({ i, offset, imageSize });
            cursor = offset + imageSize;
            consumed = i + 1;
        }

        if (batch.empty())
        {
            return consumed;
        }

        // Reuse the slot's persistent staging buffer; grow only if a batch ever needs
        // more (then it stays at that size). No per-region allocation.
        ensureStagingCapacity(*slot, cursor);

        u8* base = reinterpret_cast<u8*>(slot->staging_mapped);
        for (const BatchEntry& entry : batch)
        {
            std::memcpy(base + entry.offset, regions[entry.index].pixels, size_t(entry.size));
        }

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

        VkCommandBufferAllocateInfo allocInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = m_transferCommandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        const VkPipelineStageFlags srcStage = texture.layout_ready
            ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        VkImageMemoryBarrier toTransfer =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = texture.layout_ready
                ? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT)
                : VkAccessFlags(0),
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = texture.layout_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
            .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };

        vkCmdPipelineBarrier(commandBuffer,
            srcStage,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        for (const BatchEntry& entry : batch)
        {
            const TextureRegionUpload& region = regions[entry.index];

            VkBufferImageCopy copyRegion =
            {
                .bufferOffset = entry.offset,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = { region.x, region.y, 0 },
                .imageExtent = { u32(region.width), u32(region.height), 1 },
            };

            vkCmdCopyBufferToImage(commandBuffer, slot->staging_buffer, texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
        }

        VkImageMemoryBarrier toShader =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
            .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShader);

        vkEndCommandBuffer(commandBuffer);

        vkResetFences(m_device, 1, &slot->fence);

        VkSubmitInfo submitInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
        };

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, slot->fence);
        slot->command_buffer = commandBuffer;
        slot->pending = true;
        texture.layout_ready = true;

        return consumed;
    }

    TextureHandle VKRenderer::Impl::createTexture(int width, int height, PixelFormat format, const void* initial_data)
    {
        auto texture = std::make_unique<GpuTexture>();
        texture->width = width;
        texture->height = height;
        texture->format = format;

        const VkFormat vkFormat = toVkFormat(format);
        createImage(width, height, vkFormat,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    texture->image, texture->memory);
        createImageView(texture->image, vkFormat, texture->view);

        VkFenceCreateInfo fenceInfo =
        {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        for (UploadSlot& slot : texture->upload_slots)
        {
            vkCreateFence(m_device, &fenceInfo, nullptr, &slot.fence);
        }

        VkDescriptorSetAllocateInfo allocInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = m_descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &m_contentDescriptorSetLayout,
        };

        vkAllocateDescriptorSets(m_device, &allocInfo, &texture->descriptor);

        m_textures.push_back(std::move(texture));
        TextureHandle handle = TextureHandle(m_textures.size());

        GpuTexture& gpu = *m_textures.back();

        if (initial_data)
        {
            TextureRegionUpload region =
            {
                .x = 0,
                .y = 0,
                .width = width,
                .height = height,
                .pixels = initial_data,
            };

            submitUploadRegions(gpu, &region, 1);
        }

        return handle;
    }

    void VKRenderer::Impl::uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                         int x, int y, int width, int height, const void* pixels)
    {
        TextureRegionUpload region =
        {
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .pixels = pixels,
        };

        uploadTextureRegions(handle, format, &region, 1);
    }

    size_t VKRenderer::Impl::uploadTextureRegions(TextureHandle handle, PixelFormat format,
                                            const TextureRegionUpload* regions, size_t count)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return 0;
        }

        MANGO_UNREFERENCED(format);
        return submitUploadRegions(*texture, regions, count);
    }

    void VKRenderer::Impl::freeTextureResources(GpuTexture& texture, TextureHandle handle)
    {
        if (texture.descriptor)
        {
            vkFreeDescriptorSets(m_device, m_descriptorPool, 1, &texture.descriptor);
        }

        if (texture.view)
        {
            vkDestroyImageView(m_device, texture.view, nullptr);
        }

        if (texture.image)
        {
            vkDestroyImage(m_device, texture.image, nullptr);
        }

        if (texture.memory)
        {
            vkFreeMemory(m_device, texture.memory, nullptr);
        }

        for (UploadSlot& slot : texture.upload_slots)
        {
            if (slot.fence)
            {
                vkDestroyFence(m_device, slot.fence, nullptr);
                slot.fence = VK_NULL_HANDLE;
            }

            releaseUploadSlot(slot);
            destroyUploadSlotStaging(slot);
        }

        m_textures[handle - 1].reset();
    }

    void VKRenderer::Impl::destroyTexture(TextureHandle handle)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return;
        }

        // Blocking variant: used at teardown where waiting out in-flight uploads is
        // acceptable and guarantees the GPU no longer references the image/memory.
        recycleUploadSlots(*texture, true);
        freeTextureResources(*texture, handle);
    }

    bool VKRenderer::Impl::tryDestroyTexture(TextureHandle handle)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return true;
        }

        // Reclaim any slots whose fence has already signalled, without waiting.
        recycleUploadSlots(*texture, false);

        for (const UploadSlot& slot : texture->upload_slots)
        {
            if (slot.pending)
            {
                // An upload is still referencing this image; defer destruction.
                return false;
            }
        }

        freeTextureResources(*texture, handle);
        return true;
    }

    void VKRenderer::Impl::createPipelines()
    {
        destroyPipelines();

        const VkFormat swapchainFormat = m_swapchain->getFormat();

        m_pipelineBilinearBlend = createGraphicsPipeline(m_device, kProcessingFormat, m_processingPipelineLayout,
            m_processingVertexShader, m_processingFragmentShaderBilinear, true);

        m_pipelineBilinearNoBlend = createGraphicsPipeline(m_device, kProcessingFormat, m_processingPipelineLayout,
            m_processingVertexShader, m_processingFragmentShaderBilinear, false);

        m_pipelineBicubicBlend = createGraphicsPipeline(m_device, kProcessingFormat, m_processingPipelineLayout,
            m_processingVertexShader, m_processingFragmentShaderBicubic, true);

        m_pipelineBicubicNoBlend = createGraphicsPipeline(m_device, kProcessingFormat, m_processingPipelineLayout,
            m_processingVertexShader, m_processingFragmentShaderBicubic, false);

        m_resolvePipeline = createGraphicsPipeline(m_device, swapchainFormat, m_resolvePipelineLayout,
            m_resolveVertexShader, m_resolveFragmentShader, false);
    }

    void VKRenderer::Impl::destroyProcessingTarget()
    {
        if (m_resolveDescriptor && m_descriptorPool)
        {
            vkFreeDescriptorSets(m_device, m_descriptorPool, 1, &m_resolveDescriptor);
            m_resolveDescriptor = VK_NULL_HANDLE;
        }

        if (m_processingView)
        {
            vkDestroyImageView(m_device, m_processingView, nullptr);
            m_processingView = VK_NULL_HANDLE;
        }

        if (m_processingImage)
        {
            vkDestroyImage(m_device, m_processingImage, nullptr);
            m_processingImage = VK_NULL_HANDLE;
        }

        if (m_processingMemory)
        {
            vkFreeMemory(m_device, m_processingMemory, nullptr);
            m_processingMemory = VK_NULL_HANDLE;
        }

        m_processingLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void VKRenderer::Impl::createProcessingTarget()
    {
        destroyProcessingTarget();

        if (!m_swapchain)
        {
            return;
        }

        const VkExtent2D extent = m_swapchain->getExtent();
        if (extent.width == 0 || extent.height == 0)
        {
            return;
        }

        createImage(int(extent.width), int(extent.height), kProcessingFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    m_processingImage, m_processingMemory);
        createImageView(m_processingImage, kProcessingFormat, m_processingView);

        if (!m_resolveDescriptor && m_descriptorPool && m_resolveDescriptorSetLayout)
        {
            VkDescriptorSetAllocateInfo allocInfo =
            {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = m_descriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &m_resolveDescriptorSetLayout,
            };

            vkAllocateDescriptorSets(m_device, &allocInfo, &m_resolveDescriptor);
        }

        if (m_resolveDescriptor)
        {
            VkDescriptorImageInfo imageInfo =
            {
                .sampler = m_samplerLinear,
                .imageView = m_processingView,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };

            VkWriteDescriptorSet descriptorWrite =
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = m_resolveDescriptor,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &imageInfo,
            };

            vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
        }

        m_processingExtent = extent;
    }

    void VKRenderer::Impl::ensureProcessingTarget()
    {
        if (!m_swapchain)
        {
            return;
        }

        const VkExtent2D extent = m_swapchain->getExtent();
        if (extent.width == 0 || extent.height == 0)
        {
            return;
        }

        if (m_processingImage != VK_NULL_HANDLE &&
            extent.width == m_processingExtent.width &&
            extent.height == m_processingExtent.height)
        {
            // Already matches the swapchain extent — nothing to rebuild. Command buffers
            // depend only on the (stable) image count, and the pipelines use dynamic
            // viewport/scissor with a stable format, so the extent-sized processing
            // target is the only resource a resize touches.
            return;
        }

        // beginFrame()'s recreate already waited on all swapchain fences, but the
        // processing image may still be referenced by an earlier submission; idle the
        // device before replacing it.
        vkDeviceWaitIdle(m_device);
        createProcessingTarget();
    }

    void VKRenderer::Impl::createCommandBuffers()
    {
        const u32 imageCount = m_swapchain->getImageCount();
        m_commandBuffers.resize(imageCount);

        VkCommandBufferAllocateInfo allocInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = m_graphicsCommandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = imageCount,
        };

        vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data());
    }

    void VKRenderer::Impl::createShaders()
    {
        Compiler compiler;

        const std::string processingVertexSource = shaders::processingVertexShader();
        const std::string processingBilinearSource = shaders::fragmentShaderProcessingBilinear();
        const std::string processingBicubicSource = shaders::fragmentShaderProcessingBicubic();
        const std::string resolveVertexSource = shaders::resolveVertexShader();
        const std::string resolveFragmentSource = shaders::resolveFragmentShader();

        Shader processingVertexShader = compiler.compile(processingVertexSource.c_str(), ShaderStage::Vertex);
        Shader processingBilinear = compiler.compile(processingBilinearSource.c_str(), ShaderStage::Fragment);
        Shader processingBicubic = compiler.compile(processingBicubicSource.c_str(), ShaderStage::Fragment);
        Shader resolveVertexShader = compiler.compile(resolveVertexSource.c_str(), ShaderStage::Vertex);
        Shader resolveFragmentShader = compiler.compile(resolveFragmentSource.c_str(), ShaderStage::Fragment);

        if (!processingVertexShader.valid() || !processingBilinear.valid() || !processingBicubic.valid()
            || !resolveVertexShader.valid() || !resolveFragmentShader.valid())
        {
            printLine(Print::Error, "VKRenderer: shader compilation failed.");
            return;
        }

        m_processingVertexShader = Compiler::createShaderModule(m_device, processingVertexShader);
        m_processingFragmentShaderBilinear = Compiler::createShaderModule(m_device, processingBilinear);
        m_processingFragmentShaderBicubic = Compiler::createShaderModule(m_device, processingBicubic);
        m_resolveVertexShader = Compiler::createShaderModule(m_device, resolveVertexShader);
        m_resolveFragmentShader = Compiler::createShaderModule(m_device, resolveFragmentShader);
    }

    void VKRenderer::Impl::createSamplers()
    {
        VkSamplerCreateInfo samplerInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };

        vkCreateSampler(m_device, &samplerInfo, nullptr, &m_samplerNearest);

        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        vkCreateSampler(m_device, &samplerInfo, nullptr, &m_samplerLinear);
    }

    void VKRenderer::Impl::createDescriptorResources()
    {
        VkDescriptorSetLayoutBinding contentBinding =
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };

        VkDescriptorSetLayoutCreateInfo contentLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &contentBinding,
        };

        vkCreateDescriptorSetLayout(m_device, &contentLayoutInfo, nullptr, &m_contentDescriptorSetLayout);

        VkDescriptorSetLayoutBinding resolveBinding =
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };

        VkDescriptorSetLayoutCreateInfo resolveLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &resolveBinding,
        };

        vkCreateDescriptorSetLayout(m_device, &resolveLayoutInfo, nullptr, &m_resolveDescriptorSetLayout);

        VkPushConstantRange processingPushRange =
        {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(ProcessingPushConstants),
        };

        VkPipelineLayoutCreateInfo processingLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &m_contentDescriptorSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &processingPushRange,
        };

        vkCreatePipelineLayout(m_device, &processingLayoutInfo, nullptr, &m_processingPipelineLayout);

        VkPushConstantRange resolvePushRange =
        {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(ResolvePushConstants),
        };

        VkPipelineLayoutCreateInfo resolvePipelineLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &m_resolveDescriptorSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &resolvePushRange,
        };

        vkCreatePipelineLayout(m_device, &resolvePipelineLayoutInfo, nullptr, &m_resolvePipelineLayout);

        VkDescriptorPoolSize poolSize =
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = texture_cache_size * 4 + 1,
        };

        VkDescriptorPoolCreateInfo poolInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = texture_cache_size * 4 + 1,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize,
        };

        vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
    }

    void VKRenderer::Impl::createGeometry()
    {
        const float vertices[] =
        {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
        };

        createBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     m_vertexBuffer, m_vertexBufferMemory);

        void* data = nullptr;
        vkMapMemory(m_device, m_vertexBufferMemory, 0, sizeof(vertices), 0, &data);
        std::memcpy(data, vertices, sizeof(vertices));
        vkUnmapMemory(m_device, m_vertexBufferMemory);
    }

    void VKRenderer::Impl::destroyPipelines()
    {
        auto destroy = [this] (VkPipeline& pipeline)
        {
            if (pipeline)
            {
                vkDestroyPipeline(m_device, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
        };

        destroy(m_pipelineBilinearBlend);
        destroy(m_pipelineBilinearNoBlend);
        destroy(m_pipelineBicubicBlend);
        destroy(m_pipelineBicubicNoBlend);
        destroy(m_resolvePipeline);
    }

    VkSampler VKRenderer::Impl::selectSampler(TextureFilter filter) const
    {
        switch (filter)
        {
            case TextureFilter::NEAREST:
                return m_samplerNearest;

            case TextureFilter::BILINEAR:
            case TextureFilter::BICUBIC:
                return m_samplerLinear;
        }

        return m_samplerLinear;
    }

    VkPipeline VKRenderer::Impl::selectPipeline(const ImageDrawRequest& request) const
    {
        const bool bicubic = request.filter == TextureFilter::BICUBIC;

        if (m_blend_enabled)
        {
            return bicubic ? m_pipelineBicubicBlend : m_pipelineBilinearBlend;
        }

        return bicubic ? m_pipelineBicubicNoBlend : m_pipelineBilinearNoBlend;
    }

    bool VKRenderer::Impl::beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend)
    {
        m_clear_color[0] = clear_r;
        m_clear_color[1] = clear_g;
        m_clear_color[2] = clear_b;
        m_clear_color[3] = clear_a;
        m_blend_enabled = blend;
        m_command_buffer_recording = false;
        m_rendering_active = false;
        m_processing_rendering_active = false;
        m_content_drawn = false;

        // beginFrame() owns the swapchain recreate + suboptimal retry, so it always
        // hands back a correctly-sized image (or an empty frame to drop). We then sync
        // the only extent-sized resource we own — the processing target — to the now
        // final extent; it rebuilds only when the size actually changed.
        m_frame = m_swapchain->beginFrame();
        if (m_frame)
        {
            ensureProcessingTarget();
        }

        m_frame_active = bool(m_frame);
        return m_frame_active;
    }

    void VKRenderer::Impl::beginCommandBufferRecording()
    {
        if (!m_frame_active || m_command_buffer_recording)
        {
            return;
        }

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        vkResetCommandBuffer(commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        vkBeginCommandBuffer(commandBuffer, &beginInfo);
        m_command_buffer_recording = true;
    }

    void VKRenderer::Impl::beginProcessingRendering()
    {
        if (!m_frame_active || m_processing_rendering_active || !m_processingView)
        {
            return;
        }

        beginCommandBufferRecording();

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        const VkPipelineStageFlags srcStage = m_processingLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

        const VkAccessFlags srcAccess = m_processingLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VkAccessFlags(0)
            : VK_ACCESS_SHADER_READ_BIT;

        VkImageMemoryBarrier processingBarrier =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = srcAccess,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = m_processingLayout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_processingImage,
            .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };

        vkCmdPipelineBarrier(commandBuffer,
            srcStage,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &processingBarrier);

        m_processingLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkRenderingAttachmentInfo colorAttachment =
        {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = m_processingView,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = { { m_clear_color[0], m_clear_color[1], m_clear_color[2], m_clear_color[3] } } },
        };

        VkRenderingInfo renderingInfo =
        {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = { .extent = m_swapchain->getExtent() },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment,
        };

        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        setDynamicViewportScissor(commandBuffer);
        m_processing_rendering_active = true;
    }

    void VKRenderer::Impl::endProcessingRendering()
    {
        if (!m_frame_active || !m_processing_rendering_active)
        {
            return;
        }

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        vkCmdEndRendering(commandBuffer);
        m_processing_rendering_active = false;

        VkImageMemoryBarrier processingBarrier =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = m_processingImage,
            .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &processingBarrier);

        m_processingLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    void VKRenderer::Impl::setDynamicViewportScissor(VkCommandBuffer commandBuffer) const
    {
        const VkExtent2D extent = m_swapchain->getExtent();

        VkViewport viewport =
        {
            .x = 0.0f,
            .y = 0.0f,
            .width = float(extent.width),
            .height = float(extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        VkRect2D scissor =
        {
            .offset = { 0, 0 },
            .extent = extent,
        };

        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void VKRenderer::Impl::recordResolve()
    {
        if (!m_frame_active || !m_processingView || !m_resolvePipeline || !m_resolveDescriptor)
        {
            return;
        }

        beginSwapchainRendering();

        if (!m_rendering_active)
        {
            return;
        }

        ResolvePushConstants push {};
        push.outputTransform = float(m_output_transform);
        push.sdrWhiteNits = m_sdr_white_nits;

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipelineLayout,
            0, 1, &m_resolveDescriptor, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_resolvePipelineLayout,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResolvePushConstants), &push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffer, &offset);
        vkCmdDraw(commandBuffer, 4, 1, 0, 0);
    }

    void VKRenderer::Impl::beginSwapchainRendering()
    {
        if (!m_frame_active || m_rendering_active)
        {
            return;
        }

        beginCommandBufferRecording();

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        m_swapchain->cmdTransitionImageToColorAttachment(commandBuffer, imageIndex);

        VkRenderingAttachmentInfo colorAttachment =
        {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = m_frame.imageView(),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = { { m_clear_color[0], m_clear_color[1], m_clear_color[2], m_clear_color[3] } } },
        };

        VkRenderingInfo renderingInfo =
        {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = { .extent = m_swapchain->getExtent() },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment,
        };

        vkCmdBeginRendering(commandBuffer, &renderingInfo);
        setDynamicViewportScissor(commandBuffer);
        m_rendering_active = true;
    }

    void VKRenderer::Impl::endSwapchainRendering()
    {
        if (!m_frame_active || !m_rendering_active)
        {
            return;
        }

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        vkCmdEndRendering(commandBuffer);
        m_rendering_active = false;
    }

    void VKRenderer::Impl::recordDraw(const ImageDrawRequest& request)
    {
        GpuTexture* texture = getTexture(request.texture);
        if (!texture || !m_frame_active)
        {
            return;
        }

        recycleUploadSlots(*texture, false);

        if (!texture->layout_ready)
        {
            return;
        }

        VkSampler sampler = selectSampler(request.filter);

        VkDescriptorImageInfo imageInfo =
        {
            .sampler = sampler,
            .imageView = texture->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        VkWriteDescriptorSet descriptorWrite =
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture->descriptor,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfo,
        };

        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);

        beginProcessingRendering();

        if (!m_processing_rendering_active)
        {
            return;
        }

        m_content_drawn = true;

        ProcessingPushConstants push {};
        push.transform[0] = request.translate.x;
        push.transform[1] = request.translate.y;
        push.transform[2] = request.scale.x;
        push.transform[3] = request.scale.y;
        push.texScale[0] = 1.0f / float(std::max(1, request.width));
        push.texScale[1] = 1.0f / float(std::max(1, request.height));

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, selectPipeline(request));
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_processingPipelineLayout,
            0, 1, &texture->descriptor, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_processingPipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ProcessingPushConstants), &push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffer, &offset);
        vkCmdDraw(commandBuffer, 4, 1, 0, 0);
    }

    void VKRenderer::Impl::drawImage(const ImageDrawRequest& request)
    {
        recordDraw(request);
    }

    void VKRenderer::Impl::endFrame()
    {
        if (!m_frame_active)
        {
            return;
        }

        if (m_content_drawn)
        {
            endProcessingRendering();
            recordResolve();
        }
        else
        {
            // Scene-linear clear must go through the resolve pass so empty-window
            // background matches the encoded grey used when an image is shown.
            beginProcessingRendering();
            if (m_processing_rendering_active)
            {
                endProcessingRendering();
                recordResolve();
            }
            else if (!m_rendering_active)
            {
                beginSwapchainRendering();
            }
        }

        if (!m_rendering_active)
        {
            return;
        }

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        endSwapchainRendering();
        m_swapchain->cmdTransitionImageToPresent(commandBuffer, imageIndex);

        vkEndCommandBuffer(commandBuffer);
        m_command_buffer_recording = false;

        m_frame.submitAndPresent(m_graphicsQueue, commandBuffer);
        m_frame_active = false;
    }

    VKRenderer::VKRenderer(VulkanWindow& window, Instance& instance, VkSurfaceKHR surface)
        : m_impl(std::make_unique<Impl>(window, instance, surface))
    {
    }

    VKRenderer::~VKRenderer() = default;

    void VKRenderer::initialize() { m_impl->initialize(); }
    void VKRenderer::resize(int width, int height) { m_impl->resize(width, height); }
    bool VKRenderer::beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend) { return m_impl->beginFrame(clear_r, clear_g, clear_b, clear_a, blend); }
    void VKRenderer::drawImage(const ImageDrawRequest& request) { m_impl->drawImage(request); }
    void VKRenderer::endFrame() { m_impl->endFrame(); }
    int VKRenderer::getMaxTextureDimension() const { return m_impl->getMaxTextureDimension(); }
    TextureHandle VKRenderer::createTexture(int width, int height, PixelFormat format, const void* initial_data) { return m_impl->createTexture(width, height, format, initial_data); }
    void VKRenderer::uploadTextureRegion(TextureHandle handle, PixelFormat format, int x, int y, int width, int height, const void* pixels) { m_impl->uploadTextureRegion(handle, format, x, y, width, height, pixels); }
    size_t VKRenderer::uploadTextureRegions(TextureHandle handle, PixelFormat format, const TextureRegionUpload* regions, size_t count) { return m_impl->uploadTextureRegions(handle, format, regions, count); }
    void VKRenderer::destroyTexture(TextureHandle handle) { m_impl->destroyTexture(handle); }
    bool VKRenderer::tryDestroyTexture(TextureHandle handle) { return m_impl->tryDestroyTexture(handle); }

} // namespace ifap
