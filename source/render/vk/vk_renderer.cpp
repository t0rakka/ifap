/*
    iFap Image Viewer Example for MANGO
    Copyright 2013-2025 Twilight 3D Finland Oy. All rights reserved.
*/
#include "vk_renderer.hpp"
#include "../image_shaders.hpp"

#include <algorithm>
#include <cstring>
#include <cstddef>

#include <mango/vulkan/vulkan.hpp>
#include <mango/vulkan/compiler.hpp>

namespace ifap
{
    using namespace mango;
    using namespace mango::vulkan;

    namespace
    {
        struct PushConstants
        {
            float transform[4];
            float scale;
            float pad0;
            float texScale[2];
            float outputSrgbEncode;
        };

        static_assert(offsetof(PushConstants, scale) == 16);
        static_assert(offsetof(PushConstants, texScale) == 24);
        static_assert(offsetof(PushConstants, outputSrgbEncode) == 32);
        static_assert(sizeof(PushConstants) == 36);

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
                                          VkExtent2D extent, bool blend)
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

            VkViewport viewport =
            {
                .width = float(extent.width),
                .height = float(extent.height),
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
            };

            VkRect2D scissor =
            {
                .extent = extent,
            };

            VkPipelineViewportStateCreateInfo viewportState =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .viewportCount = 1,
                .pViewports = &viewport,
                .scissorCount = 1,
                .pScissors = &scissor,
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
        VkShaderModule m_vertexShader = VK_NULL_HANDLE;
        VkShaderModule m_fragmentShaderBilinear = VK_NULL_HANDLE;
        VkShaderModule m_fragmentShaderBicubic = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkPipeline m_pipelineBilinearBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBilinearNoBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBicubicBlend = VK_NULL_HANDLE;
        VkPipeline m_pipelineBicubicNoBlend = VK_NULL_HANDLE;
        VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
        VkSampler m_samplerNearest = VK_NULL_HANDLE;
        VkSampler m_samplerLinear = VK_NULL_HANDLE;

        struct GpuTexture
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet descriptor = VK_NULL_HANDLE;
            VkFence upload_fence = VK_NULL_HANDLE;
            VkCommandBuffer upload_command_buffer = VK_NULL_HANDLE;
            VkBuffer staging_buffer = VK_NULL_HANDLE;
            VkDeviceMemory staging_memory = VK_NULL_HANDLE;
            PixelFormat format = PixelFormat::RGBA8_UNORM;
            int width = 0;
            int height = 0;
            bool upload_pending = false;
            bool layout_ready = false;
        };

        std::vector<std::unique_ptr<GpuTexture>> m_textures;
        Swapchain::Frame m_frame;
        bool m_frame_active = false;
        bool m_command_buffer_recording = false;
        bool m_rendering_active = false;
        bool m_blend_enabled = true;
        bool m_swapchain_srgb_format = false;
        int m_max_texture_dimension = 0;
        float m_clear_color[4] = { 0.06f, 0.06f, 0.06f, 1.0f };

        void createDevice(VkInstance instance);
        void createPipelines();
        void destroyPipelines();
        void createShaders();
        void createGeometry();
        void createSamplers();
        void createDescriptorResources();
        void createCommandBuffers();
        void rebuildSwapchainResources();
        void updateSwapchain();
        GpuTexture* getTexture(TextureHandle handle);
        const GpuTexture* getTexture(TextureHandle handle) const;
        static VkFormat toVkFormat(PixelFormat format);
        static size_t bytesPerPixel(PixelFormat format);
        uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
        void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                          VkBuffer& buffer, VkDeviceMemory& memory) const;
        void createImage(int width, int height, VkFormat format, VkImage& image, VkDeviceMemory& memory) const;
        void createImageView(VkImage image, VkFormat format, VkImageView& view) const;
        void releaseStaging(GpuTexture& texture);
        void releaseUploadCommandBuffer(GpuTexture& texture);
        void waitForUpload(GpuTexture& texture);
        void submitUpload(GpuTexture& texture, int x, int y, int width, int height, const void* pixels);
        VkSampler selectSampler(TextureFilter filter) const;
        VkPipeline selectPipeline(const ImageDrawRequest& request) const;
        void recordDraw(const ImageDrawRequest& request);

        Impl(VulkanWindow& window, Instance& instance, VkSurfaceKHR surface);
        ~Impl();

        void initialize();
        void beginUploads();
        void resize(int width, int height);
        void beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend);
        void beginSwapchainRendering();
        void endSwapchainRendering();
        void drawImage(const ImageDrawRequest& request);
        void endFrame();
        TextureHandle createTexture(int width, int height, PixelFormat format, const void* initial_data);
        void uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                 int x, int y, int width, int height, const void* pixels);
        void destroyTexture(TextureHandle handle);
        int getMaxTextureDimension() const;
    };

    VKRenderer::Impl::Impl(VulkanWindow& window, Instance& instance, VkSurfaceKHR surface)
        : m_window(window)
    {
        createDevice(instance);

        const VkSurfaceFormatKHR preferredFormats[] =
        {
            {
                .format = VK_FORMAT_B8G8R8A8_SRGB,
                .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            },
            {
                .format = VK_FORMAT_B8G8R8A8_UNORM,
                .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            },
        };

        std::vector<VkSurfaceFormatKHR> surfaceFormats = getSurfaceFormats(m_physicalDevice, surface);

        VkSurfaceFormatKHR selectedFormat = surfaceFormats.empty()
            ? preferredFormats[1]
            : surfaceFormats[0];

        bool formatFound = false;

        for (const VkSurfaceFormatKHR& preferred : preferredFormats)
        {
            for (const VkSurfaceFormatKHR& format : surfaceFormats)
            {
                if (format.format == preferred.format &&
                    format.colorSpace == preferred.colorSpace)
                {
                    selectedFormat = format;
                    formatFound = true;
                    break;
                }
            }

            if (formatFound)
            {
                break;
            }
        }

        m_swapchain_srgb_format = isSrgbSurfaceFormat(selectedFormat.format);

        printLine(Print::Info, "VKRenderer: swapchain {} | {} (sRGB encode on write: {})",
            getString(selectedFormat.format),
            getString(selectedFormat.colorSpace),
            m_swapchain_srgb_format ? "hardware" : "shader");

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

            destroyPipelines();

            if (m_descriptorPool)
            {
                vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            }

            if (m_descriptorSetLayout)
            {
                vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
            }

            if (m_pipelineLayout)
            {
                vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            }

            if (m_vertexShader)
            {
                vkDestroyShaderModule(m_device, m_vertexShader, nullptr);
            }

            if (m_fragmentShaderBilinear)
            {
                vkDestroyShaderModule(m_device, m_fragmentShaderBilinear, nullptr);
            }

            if (m_fragmentShaderBicubic)
            {
                vkDestroyShaderModule(m_device, m_fragmentShaderBicubic, nullptr);
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

    void VKRenderer::Impl::beginUploads()
    {
        if (m_device != VK_NULL_HANDLE && m_graphicsQueue != VK_NULL_HANDLE)
        {
            vkQueueWaitIdle(m_graphicsQueue);
        }
    }

    int VKRenderer::Impl::getMaxTextureDimension() const
    {
        return m_max_texture_dimension;
    }

    void VKRenderer::Impl::resize(int width, int height)
    {
        MANGO_UNREFERENCED(width);
        MANGO_UNREFERENCED(height);
        updateSwapchain();
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

    void VKRenderer::Impl::createImage(int width, int height, VkFormat format, VkImage& image, VkDeviceMemory& memory) const
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
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
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

    void VKRenderer::Impl::releaseStaging(GpuTexture& texture)
    {
        if (texture.staging_buffer)
        {
            vkDestroyBuffer(m_device, texture.staging_buffer, nullptr);
            texture.staging_buffer = VK_NULL_HANDLE;
        }

        if (texture.staging_memory)
        {
            vkFreeMemory(m_device, texture.staging_memory, nullptr);
            texture.staging_memory = VK_NULL_HANDLE;
        }
    }

    void VKRenderer::Impl::releaseUploadCommandBuffer(GpuTexture& texture)
    {
        if (texture.upload_command_buffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(m_device, m_transferCommandPool, 1, &texture.upload_command_buffer);
            texture.upload_command_buffer = VK_NULL_HANDLE;
        }
    }

    void VKRenderer::Impl::waitForUpload(GpuTexture& texture)
    {
        if (!texture.upload_pending)
        {
            return;
        }

        VkResult result = vkWaitForFences(m_device, 1, &texture.upload_fence, VK_TRUE, UINT64_MAX);
        texture.upload_pending = false;

        if (result == VK_SUCCESS)
        {
            releaseUploadCommandBuffer(texture);
            releaseStaging(texture);
        }
        else if (result != VK_ERROR_DEVICE_LOST)
        {
            printLine(Print::Error, "VKRenderer: upload fence wait failed: {}", getString(result));
        }
    }

    void VKRenderer::Impl::submitUpload(GpuTexture& texture, int x, int y, int width, int height, const void* pixels)
    {
        if (!pixels || width <= 0 || height <= 0)
        {
            return;
        }

        waitForUpload(texture);

        if (texture.layout_ready)
        {
            vkQueueWaitIdle(m_graphicsQueue);
        }

        if (x < 0 || y < 0 ||
            x + width > texture.width ||
            y + height > texture.height)
        {
            printLine(Print::Error, "VKRenderer: upload rect out of bounds ({}x{} at {},{} in {}x{})",
                width, height, x, y, texture.width, texture.height);
            return;
        }

        const size_t rowBytes = size_t(width) * bytesPerPixel(texture.format);
        const VkDeviceSize imageSize = rowBytes * size_t(height);

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingMemory);

        void* mapped = nullptr;
        vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);

        const size_t srcStride = rowBytes;
        const size_t dstStride = rowBytes;
        const u8* src = static_cast<const u8*>(pixels);

        if (srcStride == dstStride)
        {
            std::memcpy(mapped, src, imageSize);
        }
        else
        {
            u8* dst = static_cast<u8*>(mapped);
            for (int row = 0; row < height; ++row)
            {
                std::memcpy(dst + row * dstStride, src + row * srcStride, rowBytes);
            }
        }

        vkUnmapMemory(m_device, stagingMemory);

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

        VkBufferImageCopy region =
        {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = { x, y, 0 },
            .imageExtent = { u32(width), u32(height), 1 },
        };

        vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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

        vkResetFences(m_device, 1, &texture.upload_fence);

        VkSubmitInfo submitInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &commandBuffer,
        };

        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, texture.upload_fence);
        texture.upload_pending = true;
        texture.layout_ready = true;
        texture.upload_command_buffer = commandBuffer;
        texture.staging_buffer = stagingBuffer;
        texture.staging_memory = stagingMemory;
    }

    TextureHandle VKRenderer::Impl::createTexture(int width, int height, PixelFormat format, const void* initial_data)
    {
        auto texture = std::make_unique<GpuTexture>();
        texture->width = width;
        texture->height = height;
        texture->format = format;

        const VkFormat vkFormat = toVkFormat(format);
        createImage(width, height, vkFormat, texture->image, texture->memory);
        createImageView(texture->image, vkFormat, texture->view);

        VkFenceCreateInfo fenceInfo =
        {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        vkCreateFence(m_device, &fenceInfo, nullptr, &texture->upload_fence);

        VkDescriptorSetAllocateInfo allocInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = m_descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &m_descriptorSetLayout,
        };

        vkAllocateDescriptorSets(m_device, &allocInfo, &texture->descriptor);

        m_textures.push_back(std::move(texture));
        TextureHandle handle = TextureHandle(m_textures.size());

        GpuTexture& gpu = *m_textures.back();

        if (initial_data)
        {
            submitUpload(gpu, 0, 0, width, height, initial_data);
        }

        return handle;
    }

    void VKRenderer::Impl::uploadTextureRegion(TextureHandle handle, PixelFormat format,
                                         int x, int y, int width, int height, const void* pixels)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return;
        }

        MANGO_UNREFERENCED(format);
        submitUpload(*texture, x, y, width, height, pixels);
    }

    void VKRenderer::Impl::destroyTexture(TextureHandle handle)
    {
        GpuTexture* texture = getTexture(handle);
        if (!texture)
        {
            return;
        }

        waitForUpload(*texture);

        vkQueueWaitIdle(m_graphicsQueue);

        if (texture->descriptor)
        {
            vkFreeDescriptorSets(m_device, m_descriptorPool, 1, &texture->descriptor);
        }

        if (texture->view)
        {
            vkDestroyImageView(m_device, texture->view, nullptr);
        }

        if (texture->image)
        {
            vkDestroyImage(m_device, texture->image, nullptr);
        }

        if (texture->memory)
        {
            vkFreeMemory(m_device, texture->memory, nullptr);
        }

        if (texture->upload_fence)
        {
            vkDestroyFence(m_device, texture->upload_fence, nullptr);
        }

        m_textures[handle - 1].reset();
    }

    void VKRenderer::Impl::createPipelines()
    {
        destroyPipelines();

        const VkExtent2D extent = m_swapchain->getExtent();
        const VkFormat colorFormat = m_swapchain->getFormat();

        m_pipelineBilinearBlend = createGraphicsPipeline(m_device, colorFormat, m_pipelineLayout,
            m_vertexShader, m_fragmentShaderBilinear, extent, true);

        m_pipelineBilinearNoBlend = createGraphicsPipeline(m_device, colorFormat, m_pipelineLayout,
            m_vertexShader, m_fragmentShaderBilinear, extent, false);

        m_pipelineBicubicBlend = createGraphicsPipeline(m_device, colorFormat, m_pipelineLayout,
            m_vertexShader, m_fragmentShaderBicubic, extent, true);

        m_pipelineBicubicNoBlend = createGraphicsPipeline(m_device, colorFormat, m_pipelineLayout,
            m_vertexShader, m_fragmentShaderBicubic, extent, false);
    }

    void VKRenderer::Impl::rebuildSwapchainResources()
    {
        vkDeviceWaitIdle(m_device);

        destroyPipelines();
        createPipelines();
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

        const std::string vertexSource = shaders::vertexShader();
        const std::string fragmentBilinearSource = shaders::fragmentShaderBilinear();
        const std::string fragmentBicubicSource = shaders::fragmentShaderBicubic();

        Shader vertexShader = compiler.compile(vertexSource.c_str(), ShaderStage::Vertex);
        Shader fragmentBilinear = compiler.compile(fragmentBilinearSource.c_str(), ShaderStage::Fragment);
        Shader fragmentBicubic = compiler.compile(fragmentBicubicSource.c_str(), ShaderStage::Fragment);

        if (!vertexShader.valid() || !fragmentBilinear.valid() || !fragmentBicubic.valid())
        {
            printLine(Print::Error, "VKRenderer: shader compilation failed.");
            return;
        }

        m_vertexShader = Compiler::createShaderModule(m_device, vertexShader);
        m_fragmentShaderBilinear = Compiler::createShaderModule(m_device, fragmentBilinear);
        m_fragmentShaderBicubic = Compiler::createShaderModule(m_device, fragmentBicubic);
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
        VkDescriptorSetLayoutBinding samplerBinding =
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };

        VkDescriptorSetLayoutCreateInfo layoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &samplerBinding,
        };

        vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout);

        VkPushConstantRange pushRange =
        {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(PushConstants),
        };

        VkPipelineLayoutCreateInfo pipelineLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &m_descriptorSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushRange,
        };

        vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);

        VkDescriptorPoolSize poolSize =
        {
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = texture_cache_size * 4,
        };

        VkDescriptorPoolCreateInfo poolInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = texture_cache_size * 4,
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
    }

    void VKRenderer::Impl::updateSwapchain()
    {
        if (!m_swapchain->recreateSwapchain())
        {
            return;
        }

        rebuildSwapchainResources();
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

    void VKRenderer::Impl::beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend)
    {
        m_clear_color[0] = clear_r;
        m_clear_color[1] = clear_g;
        m_clear_color[2] = clear_b;
        m_clear_color[3] = clear_a;
        m_blend_enabled = blend;
        m_command_buffer_recording = false;
        m_rendering_active = false;

        m_frame = m_swapchain->beginFrame();
        m_frame_active = bool(m_frame);
    }

    void VKRenderer::Impl::beginSwapchainRendering()
    {
        if (!m_frame_active || m_rendering_active)
        {
            return;
        }

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        if (!m_command_buffer_recording)
        {
            vkResetCommandBuffer(commandBuffer, 0);

            VkCommandBufferBeginInfo beginInfo =
            {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };

            vkBeginCommandBuffer(commandBuffer, &beginInfo);
            m_command_buffer_recording = true;

            m_swapchain->cmdTransitionImageToColorAttachment(commandBuffer, imageIndex);
        }

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
        if (!texture || !texture->layout_ready || !m_frame_active)
        {
            return;
        }

        waitForUpload(*texture);

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

        beginSwapchainRendering();

        if (!m_rendering_active)
        {
            return;
        }

        PushConstants push {};
        push.transform[0] = request.translate.x;
        push.transform[1] = request.translate.y;
        push.transform[2] = request.scale.x;
        push.transform[3] = request.scale.y;
        push.scale = request.intensity;
        push.texScale[0] = 1.0f / float(std::max(1, request.width));
        push.texScale[1] = 1.0f / float(std::max(1, request.height));
        push.outputSrgbEncode = m_swapchain_srgb_format ? 0.0f : 1.0f;

        const u32 imageIndex = m_frame.imageIndex();
        VkCommandBuffer commandBuffer = m_commandBuffers[imageIndex];

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, selectPipeline(request));
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &texture->descriptor, 0, nullptr);
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &push);

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

        if (!m_rendering_active)
        {
            beginSwapchainRendering();
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
    void VKRenderer::beginUploads() { m_impl->beginUploads(); }
    void VKRenderer::beginFrame(float clear_r, float clear_g, float clear_b, float clear_a, bool blend) { m_impl->beginFrame(clear_r, clear_g, clear_b, clear_a, blend); }
    void VKRenderer::drawImage(const ImageDrawRequest& request) { m_impl->drawImage(request); }
    void VKRenderer::endFrame() { m_impl->endFrame(); }
    int VKRenderer::getMaxTextureDimension() const { return m_impl->getMaxTextureDimension(); }
    TextureHandle VKRenderer::createTexture(int width, int height, PixelFormat format, const void* initial_data) { return m_impl->createTexture(width, height, format, initial_data); }
    void VKRenderer::uploadTextureRegion(TextureHandle handle, PixelFormat format, int x, int y, int width, int height, const void* pixels) { m_impl->uploadTextureRegion(handle, format, x, y, width, height, pixels); }
    void VKRenderer::destroyTexture(TextureHandle handle) { m_impl->destroyTexture(handle); }

} // namespace ifap
