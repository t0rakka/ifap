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
#include <mango/vulkan/allocator.hpp>

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

        static_assert(offsetof(ProcessingPushConstants, texScale) == 16);
        static_assert(sizeof(ProcessingPushConstants) == 24);

        constexpr VkFormat kProcessingFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

        // Swapchain format/colorspace is selected by VulkanWindow (PreferHDR intent).

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
        std::unique_ptr<Allocator> m_allocator;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        uint32_t m_graphicsQueueFamilyIndex = 0;
        VkCommandPool m_transferCommandPool = VK_NULL_HANDLE;
        VkDeviceSize m_uploadBytesPerBatch = 8 * 1024 * 1024;
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

        // Content (processing-pass) descriptor sets are rewritten every frame with the
        // texture being drawn. A single set per texture is illegal: the previous frame's
        // command buffer is still pending and references it (VUID-vkUpdateDescriptorSets-
        // None-03047 -> device loss on MoltenVK). Instead we own a ring keyed to the
        // swapchain image index -- the same lifetime the swapchain already fences before
        // a command buffer is re-recorded, so the block for the acquired image is idle.
        // kContentDescriptorsPerImage allows several draws per frame (e.g. cross-fade).
        static constexpr u32 kContentDescriptorsPerImage = 8;
        std::vector<VkDescriptorSet> m_contentDescriptors;
        u32 m_contentDescriptorCursor = 0;
        VkPipeline m_pipelineBilinearBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBilinearNoBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBicubicBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBicubicNoBlend = VK_NULL_HANDLE;
        VkPipeline m_resolvePipeline = VK_NULL_HANDLE;
        VkImage m_processingImage = VK_NULL_HANDLE;
        VmaAllocation m_processingAllocation = VK_NULL_HANDLE;
        VkImageView m_processingView = VK_NULL_HANDLE;
        VkImageLayout m_processingLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExtent2D m_processingExtent { 0, 0 };
        BufferAllocation m_vertexBuffer;
        VkSampler m_samplerNearest = VK_NULL_HANDLE;
        VkSampler m_samplerLinear = VK_NULL_HANDLE;

        // Renderer-global pool of in-flight upload/clear submissions, each tagged with
        // the timeline value its submit signals. A slot is idle once that value is
        // reached; idle slots are reclaimed and their staging reused. Replaces the old
        // per-texture fenced slots — staging is now bounded globally, not per texture.
        static constexpr int kUploadSlotCount = 8;

        struct UploadSlot
        {
            VkCommandBuffer command_buffer = VK_NULL_HANDLE;
            // Persistent, persistently-mapped staging buffer reused across uploads.
            // Backed by a VMA Upload allocation; write through staging.mapped. (Clears
            // acquire a slot but use no staging, so most slots never grow one.)
            BufferAllocation staging;
            VkDeviceSize staging_capacity = 0;
            // Timeline value this slot's last submission signals; 0 = never used. The
            // slot is idle (reclaimable) once the timeline has reached this value.
            u64 pending_value = 0;
        };

        UploadSlot m_uploadSlots[kUploadSlotCount];

        struct GpuTexture
        {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            PixelFormat format = PixelFormat::RGBA8_UNORM;
            int width = 0;
            int height = 0;
            bool layout_ready = false;
            // Timeline value from the most recent upload/clear submit. The OnDemand loop
            // polls this (via isTextureUploadComplete) so a one-shot decode cannot go idle
            // before the GPU copy is visible; recordDraw does not skip — progressive
            // uploads keep showing the last good partial image.
            u64 last_upload_value = 0;
            // Highest timeline value of any submission (upload, clear, or frame draw)
            // that referenced this texture's image/view/descriptor. Destruction is
            // gated until the timeline reaches it (see tryDestroyTexture); 0 means
            // nothing has ever referenced it.
            u64 last_used_value = 0;
        };

        std::vector<std::unique_ptr<GpuTexture>> m_textures;
        Swapchain::Frame m_frame;
        bool m_frame_active = false;
        bool m_command_buffer_recording = false;
        bool m_rendering_active = false;
        bool m_processing_rendering_active = false;
        bool m_content_drawn = false;
        bool m_blend_enabled = true;
        int m_max_texture_dimension = 0;
        float m_clear_color[4] = { 0.06f, 0.06f, 0.06f, 1.0f };

        // The unified GPU clock. Every queue submission (upload, clear, frame) signals
        // ++m_timelineValue; resources record the value of their last use and are
        // reclaimed once vkGetSemaphoreCounterValue() passes it. m_frame_value is the
        // value the in-flight frame will signal, reserved in beginFrame() and stamped
        // onto every texture sampled in recordDraw().
        VkSemaphore m_timeline = VK_NULL_HANDLE;
        u64 m_timelineValue = 0;
        u64 m_frame_value = 0;

        void createContentDescriptors();
        void ensureContentDescriptors();
        Swapchain& swapchain() { return m_window.swapchain(); }
        const Swapchain& swapchain() const { return m_window.swapchain(); }
        VkCommandBuffer frameCommandBuffer(u32 imageIndex) const { return m_window.commandBuffer(imageIndex); }

        void createPipelines();
        void destroyPipelines();
        void createShaders();
        void createGeometry();
        void createSamplers();
        void createDescriptorResources();
        void createProcessingTarget();
        void destroyProcessingTarget();
        void ensureProcessingTarget();
        void beginCommandBufferRecording();
        void beginProcessingRendering();
        void endProcessingRendering();
        void setDynamicViewportScissor(VkCommandBuffer commandBuffer) const;
        void recordResolve();
        GpuTexture* getTexture(TextureHandle handle);
        const GpuTexture* getTexture(TextureHandle handle) const;
        static VkFormat toVkFormat(PixelFormat format);
        static size_t bytesPerPixel(PixelFormat format);
        ImageAllocation createImage(int width, int height, VkFormat format, VkImageUsageFlags usage) const;
        void createImageView(VkImage image, VkFormat format, VkImageView& view) const;
        void destroyUploadSlotStaging(UploadSlot& slot);
        void ensureStagingCapacity(UploadSlot& slot, VkDeviceSize size);
        u64 timelineCompleted() const;
        void waitTimeline(u64 value) const;
        u64 submitTimelined(VkCommandBuffer commandBuffer);
        UploadSlot* acquireUploadSlot();
        size_t submitUploadRegions(GpuTexture& texture, const TextureRegionUpload* regions, size_t count);
        void clearTexture(GpuTexture& texture);
        VkSampler selectSampler(TextureFilter filter) const;
        VkPipeline selectPipeline(const ImageDrawRequest& request) const;
        void recordDraw(const ImageDrawRequest& request);
        bool isTextureUploadComplete(TextureHandle handle) const;
        bool isTextureLayoutReady(TextureHandle handle) const;

        Impl(VulkanWindow& window, Instance& instance);
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
        void releaseUploadStaging(TextureHandle handle);
        void setUploadBytesPerFrame(size_t bytes);
        void freeTextureResources(GpuTexture& texture, TextureHandle handle);
        int getMaxTextureDimension() const;
    };

    VKRenderer::Impl::Impl(VulkanWindow& window, Instance& instance)
        : m_window(window)
        , m_physicalDevice(window.physicalDevice())
        , m_device(window.device())
        , m_graphicsQueue(window.graphicsQueue())
        , m_graphicsQueueFamilyIndex(window.graphicsQueueFamilyIndex())
    {
        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(m_physicalDevice, &deviceProperties);
        m_max_texture_dimension = int(deviceProperties.limits.maxImageDimension2D);

        m_allocator = std::make_unique<Allocator>(instance, m_physicalDevice, m_device, VK_API_VERSION_1_3);

        VkSemaphoreTypeCreateInfo timelineType =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };

        VkSemaphoreCreateInfo timelineInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &timelineType,
        };

        vkCreateSemaphore(m_device, &timelineInfo, nullptr, &m_timeline);

        const VkSurfaceFormatKHR selectedFormat = window.surfaceFormat();

        printLine(Print::Info, "VKRenderer: selected {} | {} (hardware sRGB: {}, output transform: {}, HDR: {}, SDR white: {} nits)",
            getString(selectedFormat.format),
            getString(selectedFormat.colorSpace),
            isSRGB(selectedFormat.format) ? "yes" : "no",
            int(selectOutputTransform(selectedFormat)),
            isHDR(selectedFormat) ? "yes" : "no",
            int(defaultSdrWhiteNits(selectedFormat.colorSpace)));

        VkCommandPoolCreateInfo poolInfo =
        {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = m_graphicsQueueFamilyIndex,
        };

        vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_transferCommandPool);

        createShaders();
        createSamplers();
        createDescriptorResources();
        createGeometry();
        createPipelines();
        createContentDescriptors();
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

            m_allocator->destroyBuffer(m_vertexBuffer);
            m_vertexBuffer = {};

            if (m_samplerNearest)
            {
                vkDestroySampler(m_device, m_samplerNearest, nullptr);
            }

            if (m_samplerLinear)
            {
                vkDestroySampler(m_device, m_samplerLinear, nullptr);
            }

            // vkDeviceWaitIdle above guarantees every timelined submission has retired,
            // so the upload ring's command buffers and staging can be freed outright.
            for (UploadSlot& slot : m_uploadSlots)
            {
                if (slot.command_buffer != VK_NULL_HANDLE)
                {
                    vkFreeCommandBuffers(m_device, m_transferCommandPool, 1, &slot.command_buffer);
                    slot.command_buffer = VK_NULL_HANDLE;
                }

                destroyUploadSlotStaging(slot);
            }

            if (m_timeline != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, m_timeline, nullptr);
                m_timeline = VK_NULL_HANDLE;
            }

            vkDestroyCommandPool(m_device, m_transferCommandPool, nullptr);

            m_allocator.reset();
        }
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

        if (!m_window.isDeviceReady())
        {
            return;
        }

        vkDeviceWaitIdle(m_device);

        if (m_descriptorPool)
        {
            vkResetDescriptorPool(m_device, m_descriptorPool, 0);
        }

        createContentDescriptors();
        createProcessingTarget();
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

    ImageAllocation VKRenderer::Impl::createImage(int width, int height, VkFormat format, VkImageUsageFlags usage) const
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

        return m_allocator->createImage(imageInfo, MemoryUsage::GpuOnly);
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

    u64 VKRenderer::Impl::timelineCompleted() const
    {
        u64 value = 0;
        vkGetSemaphoreCounterValue(m_device, m_timeline, &value);
        return value;
    }

    void VKRenderer::Impl::waitTimeline(u64 value) const
    {
        if (value == 0)
        {
            return;
        }

        VkSemaphoreWaitInfo waitInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &m_timeline,
            .pValues = &value,
        };

        vkWaitSemaphores(m_device, &waitInfo, UINT64_MAX);
    }

    u64 VKRenderer::Impl::submitTimelined(VkCommandBuffer commandBuffer)
    {
        // One submission point for all upload/clear work: signals the next timeline
        // value (no fence needed — reclamation reads the timeline). Same queue as the
        // frame, so submission order + the command buffer's own image barriers give the
        // upload→sample dependency without an explicit wait.
        const u64 value = ++m_timelineValue;

        VkTimelineSemaphoreSubmitInfo timelineInfo =
        {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &value,
        };

        VkSubmitInfo submitInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timelineInfo,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &m_timeline,
        };

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        return value;
    }

    void VKRenderer::Impl::destroyUploadSlotStaging(UploadSlot& slot)
    {
        // Persistently mapped: VMA unmaps as part of destroying the allocation.
        m_allocator->destroyBuffer(slot.staging);
        slot.staging = {};
        slot.staging_capacity = 0;
    }

    void VKRenderer::Impl::ensureStagingCapacity(UploadSlot& slot, VkDeviceSize size)
    {
        if (slot.staging.buffer != VK_NULL_HANDLE && slot.staging_capacity >= size)
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

        slot.staging = m_allocator->createBuffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                 MemoryUsage::Upload, true);
        slot.staging_capacity = capacity;
    }

    VKRenderer::Impl::UploadSlot* VKRenderer::Impl::acquireUploadSlot()
    {
        // A slot is idle once the timeline has passed the value its last submission
        // signalled (pending_value == 0 means never used). Reclaim that submission's
        // command buffer and hand the slot back; its staging is kept for reuse. If all
        // slots are still in flight, return null — the caller simply retries next frame
        // (the pipeline stays non-blocking).
        const u64 completed = timelineCompleted();

        for (UploadSlot& slot : m_uploadSlots)
        {
            if (slot.pending_value <= completed)
            {
                if (slot.command_buffer != VK_NULL_HANDLE)
                {
                    vkFreeCommandBuffers(m_device, m_transferCommandPool, 1, &slot.command_buffer);
                    slot.command_buffer = VK_NULL_HANDLE;
                }

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

        UploadSlot* slot = acquireUploadSlot();
        if (!slot)
        {
            return 0;
        }

        const VkDeviceSize maxUploadBytesPerBatch = m_uploadBytesPerBatch;
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
            if (!batch.empty() && offset + imageSize > maxUploadBytesPerBatch)
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

        u8* base = reinterpret_cast<u8*>(slot->staging.mapped);
        for (const BatchEntry& entry : batch)
        {
            std::memcpy(base + entry.offset, regions[entry.index].pixels, size_t(entry.size));
        }

        // VMA Upload memory may be non-coherent; make the writes visible to the GPU
        // before the copy. A no-op on coherent (incl. ReBAR) allocations.
        m_allocator->flush(slot->staging.allocation, 0, VK_WHOLE_SIZE);

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

            vkCmdCopyBufferToImage(commandBuffer, slot->staging.buffer, texture.image,
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

        const u64 value = submitTimelined(commandBuffer);
        slot->command_buffer = commandBuffer;
        slot->pending_value = value;
        texture.layout_ready = true;
        texture.last_upload_value = value;
        texture.last_used_value = std::max(texture.last_used_value, value);

        return consumed;
    }

    bool VKRenderer::Impl::isTextureUploadComplete(TextureHandle handle) const
    {
        const GpuTexture* texture = getTexture(handle);
        if (!texture || texture->last_upload_value == 0)
        {
            return true;
        }

        return timelineCompleted() >= texture->last_upload_value;
    }

    bool VKRenderer::Impl::isTextureLayoutReady(TextureHandle handle) const
    {
        const GpuTexture* texture = getTexture(handle);
        return texture && texture->layout_ready;
    }

    void VKRenderer::Impl::clearTexture(GpuTexture& texture)
    {
        // Fill freshly created images with a defined neutral grey before any region
        // upload arrives. The image backing a full-resolution texture is created with
        // uninitialised device memory and streamed in tile-by-tile; once the first tile
        // lands, layout_ready flips and the whole image is sampled — including the
        // not-yet-uploaded area. Desktop drivers tend to hand back zeroed (dark) memory,
        // but MoltenVK does not, so that area samples garbage and reads as magenta/pink.
        // Clearing guarantees a defined placeholder colour on every platform. Submitted
        // through the same async upload-slot path as a copy so it never blocks the caller.
        UploadSlot* slot = acquireUploadSlot();
        if (!slot)
        {
            return;
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

        const VkImageSubresourceRange range =
        {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };

        VkImageMemoryBarrier toTransfer =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
            .subresourceRange = range,
        };

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkClearColorValue clearColor {};
        clearColor.float32[0] = 0.125f;
        clearColor.float32[1] = 0.125f;
        clearColor.float32[2] = 0.125f;
        clearColor.float32[3] = 1.0f;

        vkCmdClearColorImage(commandBuffer, texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

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
            .subresourceRange = range,
        };

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShader);

        vkEndCommandBuffer(commandBuffer);

        const u64 value = submitTimelined(commandBuffer);
        slot->command_buffer = commandBuffer;
        slot->pending_value = value;
        texture.layout_ready = true;
        texture.last_upload_value = value;
        texture.last_used_value = std::max(texture.last_used_value, value);
    }

    TextureHandle VKRenderer::Impl::createTexture(int width, int height, PixelFormat format, const void* initial_data)
    {
        const VkFormat vkFormat = toVkFormat(format);
        ImageAllocation image = createImage(width, height, vkFormat,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

        if (image.image == VK_NULL_HANDLE)
        {
            // Allocation failed (typically VRAM exhaustion for very large images). Never
            // build an image view / descriptor on a null image: that produces an invalid
            // descriptor and crashes the GPU at draw time. Return 0 so the caller keeps
            // its placeholder and can retry once other textures have been freed.
            printLine(Print::Error, "VKRenderer: createTexture {}x{} failed (out of GPU memory)", width, height);
            return 0;
        }

        auto texture = std::make_unique<GpuTexture>();
        texture->width = width;
        texture->height = height;
        texture->format = format;
        texture->image = image.image;
        texture->allocation = image.allocation;
        createImageView(texture->image, vkFormat, texture->view);

        // The processing-pass (content) descriptor set is not owned per texture: it is
        // bound from a per-image ring and written with this texture's view at draw time
        // (see recordDraw). Only the image + view are owned here.

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
        else
        {
            // No initial pixels: the image streams in tile-by-tile, so clear it to a
            // defined placeholder colour first (otherwise the not-yet-uploaded area
            // samples uninitialised memory — pink on MoltenVK).
            clearTexture(gpu);
        }

        if (!gpu.layout_ready)
        {
            freeTextureResources(gpu, handle);
            m_textures.pop_back();
            return 0;
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
        if (texture.view)
        {
            vkDestroyImageView(m_device, texture.view, nullptr);
        }

        if (texture.image)
        {
            m_allocator->destroyImage({ texture.image, texture.allocation });
            texture.image = VK_NULL_HANDLE;
            texture.allocation = VK_NULL_HANDLE;
        }

        // No per-texture upload state to free: staging/command buffers live in the
        // renderer-global upload ring, reclaimed by timeline value.
        m_textures[handle - 1].reset();
    }

    void VKRenderer::Impl::destroyTexture(TextureHandle handle)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return;
        }

        // Blocking variant: used at teardown where waiting out in-flight GPU work is
        // acceptable and guarantees the GPU no longer references the image/memory.
        waitTimeline(texture->last_used_value);
        freeTextureResources(*texture, handle);
    }

    bool VKRenderer::Impl::tryDestroyTexture(TextureHandle handle)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return true;
        }

        // The image/view/descriptor may still be referenced by an in-flight upload,
        // clear, or render frame — every such submission stamped its signalled timeline
        // value into last_used_value. Freeing before the timeline reaches that value is
        // a use-after-free that faults the GPU (VK_ERROR_DEVICE_LOST). One rule covers
        // uploads and frames alike; last_used_value == 0 means nothing referenced it.
        if (texture->last_used_value > timelineCompleted())
        {
            return false;
        }

        freeTextureResources(*texture, handle);
        return true;
    }

    void VKRenderer::Impl::setUploadBytesPerFrame(size_t bytes)
    {
        m_uploadBytesPerBatch = bytes ? VkDeviceSize(bytes) : VkDeviceSize(1);
    }

    void VKRenderer::Impl::releaseUploadStaging(TextureHandle handle)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return;
        }

        // No-op now: staging lives in the renderer-global upload ring (bounded by
        // kUploadSlotCount), not per texture, so there is nothing texture-specific to
        // reclaim here. Kept so the cache can still signal "fully uploaded" intent.
        MANGO_UNREFERENCED(texture);
    }

    void VKRenderer::Impl::createPipelines()
    {
        destroyPipelines();

        const VkFormat swapchainFormat = swapchain().getFormat();

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
            m_allocator->destroyImage({ m_processingImage, m_processingAllocation });
            m_processingImage = VK_NULL_HANDLE;
            m_processingAllocation = VK_NULL_HANDLE;
        }

        m_processingLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void VKRenderer::Impl::createProcessingTarget()
    {
        destroyProcessingTarget();

        if (!m_window.isDeviceReady())
        {
            return;
        }

        const VkExtent2D extent = swapchain().getExtent();
        if (extent.width == 0 || extent.height == 0)
        {
            return;
        }

        ImageAllocation processing = createImage(int(extent.width), int(extent.height), kProcessingFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        m_processingImage = processing.image;
        m_processingAllocation = processing.allocation;
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
        if (!m_window.isDeviceReady())
        {
            return;
        }

        const VkExtent2D extent = swapchain().getExtent();
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

    void VKRenderer::Impl::createContentDescriptors()
    {
        const u32 imageCount = swapchain().getImageCount();
        const u32 contentDescriptorCount = imageCount * kContentDescriptorsPerImage;
        m_contentDescriptors.assign(contentDescriptorCount, VK_NULL_HANDLE);

        if (!m_descriptorPool || !m_contentDescriptorSetLayout || contentDescriptorCount == 0)
        {
            return;
        }

        std::vector<VkDescriptorSetLayout> layouts(contentDescriptorCount, m_contentDescriptorSetLayout);

        VkDescriptorSetAllocateInfo contentAllocInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = m_descriptorPool,
            .descriptorSetCount = contentDescriptorCount,
            .pSetLayouts = layouts.data(),
        };

        VkResult result = vkAllocateDescriptorSets(m_device, &contentAllocInfo, m_contentDescriptors.data());
        if (result != VK_SUCCESS)
        {
            printLine(Print::Error, "VKRenderer: failed to allocate content descriptor sets.");
        }
    }

    void VKRenderer::Impl::ensureContentDescriptors()
    {
        const u32 imageCount = swapchain().getImageCount();
        const u32 expected = imageCount * kContentDescriptorsPerImage;

        if (m_contentDescriptors.size() != expected)
        {
            if (m_descriptorPool)
            {
                vkResetDescriptorPool(m_device, m_descriptorPool, 0);
            }

            createContentDescriptors();
        }
    }

    void VKRenderer::Impl::createShaders()
    {
        Compiler compiler;

        const std::string processingVertexSource = shaders::processingVertexShader();
        const std::string processingBilinearSource = shaders::fragmentShaderProcessingBilinear();
        const std::string processingBicubicSource = shaders::fragmentShaderProcessingBicubic();
        const std::string resolveVertexSource = shaders::resolveVertexShader();
        const std::string resolveFragmentSource = shaders::resolveFragmentShader(
            swapchain().getOutputTransformGLSL());

        Shader processingVertexShader = compiler.compile(processingVertexSource.c_str(), ShaderStage::Vertex);
        Shader processingBilinear = compiler.compile(processingBilinearSource.c_str(), ShaderStage::Fragment);
        Shader processingBicubic = compiler.compile(processingBicubicSource.c_str(), ShaderStage::Fragment);
        Shader resolveVertexShader = compiler.compile(resolveVertexSource.c_str(), ShaderStage::Vertex);
        Shader resolveFragmentShader = compiler.compile(resolveFragmentSource.c_str(), ShaderStage::Fragment);

        if (!processingVertexShader.valid() || !processingBilinear.valid() || !processingBicubic.valid()
            || !resolveVertexShader.valid() || !resolveFragmentShader.valid())
        {
            printLine(Print::Error, "VKRenderer: shader compilation failed.");
            if (!resolveFragmentShader.valid())
            {
                printLine(Print::Error, "Resolve fragment shader:\n{}", resolveFragmentShader.log);
            }
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

        VkPipelineLayoutCreateInfo resolvePipelineLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &m_resolveDescriptorSetLayout,
        };

        vkCreatePipelineLayout(m_device, &resolvePipelineLayoutInfo, nullptr, &m_resolvePipelineLayout);

        // Sets drawn from this pool: the per-image content ring (image count *
        // kContentDescriptorsPerImage) plus the single resolve set. Textures no longer
        // own a set (the content set is bound from the ring at draw time), so this is
        // small; size generously anyway since descriptor sets are cheap.
        const u32 kMaxDescriptorSets = u32(texture_cache_size) * 8 + 32;

        VkDescriptorPoolSize poolSize =
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = kMaxDescriptorSets,
        };

        VkDescriptorPoolCreateInfo poolInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = kMaxDescriptorSets,
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

        // Static 4-vertex quad: a persistently-mapped Upload buffer written once. Flush
        // covers the non-coherent case (no-op otherwise); the GPU only reads it later.
        m_vertexBuffer = m_allocator->createBuffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                   MemoryUsage::Upload, true);
        std::memcpy(m_vertexBuffer.mapped, vertices, sizeof(vertices));
        m_allocator->flush(m_vertexBuffer.allocation, 0, VK_WHOLE_SIZE);
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
        m_frame = m_window.beginDraw();
        if (m_frame)
        {
            ensureContentDescriptors();
            ensureProcessingTarget();

            // The acquired image's prior submission has retired (the swapchain fenced it
            // before handing the frame back), so its content-descriptor block is idle and
            // safe to rewrite. Restart at the block's first slot.
            m_contentDescriptorCursor = 0;

            // Reserve the timeline value this frame's submit will signal. All upload /
            // clear work for this tick already ran (in the cache update before render),
            // so this value is strictly greater than theirs; recordDraw stamps it onto
            // every sampled texture. An aborted frame simply leaves the value unsignalled
            // (a harmless gap — nothing waits on exactly it).
            m_frame_value = ++m_timelineValue;
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
        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

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
        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

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
            .renderArea = { .extent = swapchain().getExtent() },
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
        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

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
        const VkExtent2D extent = swapchain().getExtent();

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

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipelineLayout,
            0, 1, &m_resolveDescriptor, 0, nullptr);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffer.buffer, &offset);
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
        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

        swapchain().cmdTransitionImageToColorAttachment(commandBuffer, imageIndex);

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
            .renderArea = { .extent = swapchain().getExtent() },
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
        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

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

        if (!texture->layout_ready)
        {
            return;
        }

        const u32 imageIndex = m_frame.imageIndex();

        // Take the next free set from this image's content-descriptor block. The block
        // is idle (its frame retired before acquire) and each draw within the frame uses
        // a distinct slot, so writing it never races an in-flight or already-recorded
        // draw. Clamp on the rare overflow rather than aliasing a slot already bound this
        // frame (which would make every draw sample the last texture).
        const u32 blockBase = imageIndex * kContentDescriptorsPerImage;
        const u32 slot = blockBase + std::min(m_contentDescriptorCursor, kContentDescriptorsPerImage - 1);
        if (slot >= m_contentDescriptors.size())
        {
            return;
        }
        VkDescriptorSet descriptor = m_contentDescriptors[slot];
        if (m_contentDescriptorCursor < kContentDescriptorsPerImage - 1)
        {
            ++m_contentDescriptorCursor;
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
            .dstSet = descriptor,
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

        // The in-flight frame's command buffer now references this texture's
        // image/view; stamp the value that frame will signal so tryDestroyTexture
        // won't free the texture until that frame has retired.
        texture->last_used_value = std::max(texture->last_used_value, m_frame_value);

        ProcessingPushConstants push {};
        push.transform[0] = request.translate.x;
        push.transform[1] = request.translate.y;
        push.transform[2] = request.scale.x;
        push.transform[3] = request.scale.y;
        push.texScale[0] = 1.0f / float(std::max(1, request.width));
        push.texScale[1] = 1.0f / float(std::max(1, request.height));

        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, selectPipeline(request));
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_processingPipelineLayout,
            0, 1, &descriptor, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_processingPipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ProcessingPushConstants), &push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffer.buffer, &offset);
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
        VkCommandBuffer commandBuffer = frameCommandBuffer(imageIndex);

        endSwapchainRendering();
        swapchain().cmdTransitionImageToPresent(commandBuffer, imageIndex);

        vkEndCommandBuffer(commandBuffer);
        m_command_buffer_recording = false;

        // Additionally signal the unified timeline at the value reserved in beginFrame,
        // so reclamation can retire textures this frame sampled once it completes. The
        // swapchain's own binary semaphores + fence still drive present/acquire as before.
        m_frame.submitAndPresent(m_graphicsQueue, commandBuffer, m_timeline, m_frame_value);
        m_frame_active = false;
    }

    VKRenderer::VKRenderer(VulkanWindow& window, Instance& instance)
        : m_impl(std::make_unique<Impl>(window, instance))
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
    void VKRenderer::releaseUploadStaging(TextureHandle handle) { m_impl->releaseUploadStaging(handle); }
    void VKRenderer::setUploadBytesPerFrame(size_t bytes) { m_impl->setUploadBytesPerFrame(bytes); }
    bool VKRenderer::isTextureUploadComplete(TextureHandle handle) const { return m_impl->isTextureUploadComplete(handle); }
    bool VKRenderer::isTextureLayoutReady(TextureHandle handle) const { return m_impl->isTextureLayoutReady(handle); }

} // namespace ifap
