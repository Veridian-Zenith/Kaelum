#include "renderer.hpp"
#include "glyph_engine.hpp"
#include "logger.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
#include <expected>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <algorithm>

namespace Kaelum {

Renderer::~Renderer() {
    deinit();
}

Kaelum::Expected<void, std::string> Renderer::init(const Config& cfg) {
    config_ = cfg;
    KAELUM_INFO("Initializing renderer: {}x{} cell {}x{}", cfg.width, cfg.height, cfg.cell_width, cfg.cell_height);
    
    // Create Vulkan instance
    VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Kaelum",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "Kaelum",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };
    
#ifdef KAELUM_DEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo inst_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    
#ifdef KAELUM_DEBUG
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    inst_info.enabledLayerCount = 1;
    inst_info.ppEnabledLayerNames = layers;
#endif

    VkResult res = vkCreateInstance(&inst_info, nullptr, &instance_);
    if (res != VK_SUCCESS) return Kaelum::make_unexpected("Failed to create Vulkan instance");

    // Create Wayland surface
    VkWaylandSurfaceCreateInfoKHR surf_info{
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = wayland_display_,
        .surface = static_cast<wl_surface*>(wayland_surface_),
    };
    
    auto vkCreateWaylandSurface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
    if (!vkCreateWaylandSurface || vkCreateWaylandSurface(instance_, &surf_info, nullptr, &surface_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create Wayland surface");
    }

    // Pick physical device
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance_, &dev_count, nullptr);
    if (dev_count == 0) return Kaelum::make_unexpected("No Vulkan devices found");
    
    std::vector<VkPhysicalDevice> devices(dev_count);
    vkEnumeratePhysicalDevices(instance_, &dev_count, devices.data());
    physical_device_ = devices[0];

    // Find queue family
    uint32_t qfam_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfam_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfams(qfam_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qfam_count, qfams.data());
    
    queue_family_ = UINT32_MAX;
    for (uint32_t i = 0; i < qfam_count; ++i) {
        if (qfams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            VkBool32 present = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present);
            if (present) {
                queue_family_ = i;
                break;
            }
        }
    }
    
    if (queue_family_ == UINT32_MAX) return Kaelum::make_unexpected("No suitable queue family");

    // Create logical device
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qinfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    
    std::vector<const char*> dev_ext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = VK_TRUE;
    
    VkDeviceCreateInfo dev_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qinfo,
        .enabledExtensionCount = static_cast<uint32_t>(dev_ext.size()),
        .ppEnabledExtensionNames = dev_ext.data(),
        .pEnabledFeatures = &features,
    };
    
    res = vkCreateDevice(physical_device_, &dev_info, nullptr, &device_);
    if (res != VK_SUCCESS) return Kaelum::make_unexpected("Failed to create logical device");
    
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    auto swap_res = create_swapchain();
    if (!swap_res) return swap_res;
    
    auto rp_res = create_render_pass();
    if (!rp_res) return rp_res;
    
    auto dsl_res = create_descriptor_set_layout();
    if (!dsl_res) return dsl_res;
    
    auto pipe_res = create_pipeline();
    if (!pipe_res) return pipe_res;
    
    auto fb_res = create_framebuffers();
    if (!fb_res) return fb_res;
    
    auto cp_res = create_command_pool();
    if (!cp_res) return cp_res;
    
    auto sync_res = create_sync_objects();
    if (!sync_res) return sync_res;
    
    auto at_res = create_atlas();
    if (!at_res) return at_res;
    
    auto vb_res = create_vertex_buffer();
    if (!vb_res) return vb_res;

    KAELUM_INFO("Renderer initialized");
    return {};
}

void Renderer::deinit() {
    if (!device_) return;
    
    vkDeviceWaitIdle(device_);
    
    cleanup_swapchain();
    
    if (vertex_buffer_) {
        vkDestroyBuffer(device_, vertex_buffer_, nullptr);
        vkFreeMemory(device_, vertex_buffer_memory_, nullptr);
    }
    
    if (atlas_image_) {
        vkDestroyImageView(device_, atlas_view_, nullptr);
        vkDestroyImage(device_, atlas_image_, nullptr);
        vkFreeMemory(device_, atlas_memory_, nullptr);
        vkDestroySampler(device_, atlas_sampler_, nullptr);
    }
    
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
    
    for (auto sem : image_available_semaphores_) vkDestroySemaphore(device_, sem, nullptr);
    for (auto sem : render_finished_semaphores_) vkDestroySemaphore(device_, sem, nullptr);
    for (auto fence : in_flight_fences_) vkDestroyFence(device_, fence, nullptr);
    
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

void Renderer::cleanup_swapchain() {
    for (auto fb : framebuffers_) {
        if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();
    
    for (auto view : swapchain_image_views_) {
        if (view) vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.clear();
    
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

Kaelum::Expected<void, std::string> Renderer::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());
    
    VkSurfaceFormatKHR chosen = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(config_.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(config_.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    
    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount) img_count = caps.maxImageCount;
    
    VkSwapchainCreateInfoKHR swap_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_,
        .minImageCount = img_count,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    
    if (vkCreateSwapchainKHR(device_, &swap_info, nullptr, &swapchain_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create swapchain");
    }
    
    uint32_t img_count2 = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count2, nullptr);
    swapchain_images_.resize(img_count2);
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count2, swapchain_images_.data());
    
    swapchain_format_ = chosen.format;
    swapchain_extent_ = extent;
    
    swapchain_image_views_.resize(img_count2);
    for (size_t i = 0; i < img_count2; ++i) {
        VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchain_images_[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchain_format_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(device_, &view_info, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
            return Kaelum::make_unexpected("Failed to create image view");
        }
    }
    
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_render_pass() {
    VkAttachmentDescription color_att{
        .format = swapchain_format_,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    
    VkAttachmentReference color_ref{.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    
    VkSubpassDescription subpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };
    
    VkSubpassDependency dep{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    
    VkRenderPassCreateInfo rp_info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_att,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    
    if (vkCreateRenderPass(device_, &rp_info, nullptr, &render_pass_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create render pass");
    }
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_descriptor_set_layout() {
    VkDescriptorSetLayoutBinding layout_binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr,
    };
    
    VkDescriptorSetLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &layout_binding,
    };
    
    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create descriptor set layout");
    }
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_pipeline() {
    auto read_shader = [](const char* path) -> std::vector<uint32_t> {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return {};
        size_t size = file.tellg();
        file.seekg(0);
        std::vector<uint32_t> code(size / sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(code.data()), size);
        return code;
    };
    
    auto vert_code = read_shader("shaders/glyph.vert.spv");
    auto frag_code = read_shader("shaders/glyph.frag.spv");
    
    if (vert_code.empty() || frag_code.empty()) {
        return Kaelum::make_unexpected("Failed to load shader SPIR-V");
    }
    
    VkShaderModuleCreateInfo vert_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vert_code.size() * sizeof(uint32_t),
        .pCode = vert_code.data(),
    };
    VkShaderModule vert_module;
    vkCreateShaderModule(device_, &vert_info, nullptr, &vert_module);
    
    VkShaderModuleCreateInfo frag_info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = frag_code.size() * sizeof(uint32_t),
        .pCode = frag_code.data(),
    };
    VkShaderModule frag_module;
    vkCreateShaderModule(device_, &frag_info, nullptr, &frag_module);
    
    VkPipelineShaderStageCreateInfo stages[2]{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert_module, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag_module, .pName = "main"},
    };
    
    VkVertexInputBindingDescription binding{.binding = 0, .stride = 4 * sizeof(float) + 2 * sizeof(float) + 4 * sizeof(float), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    
    VkVertexInputAttributeDescription attrs[3]{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 4 * sizeof(float)},
    };
    
    VkPipelineVertexInputStateCreateInfo vi{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attrs,
    };
    
    VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    
    VkViewport viewport{0, 0, static_cast<float>(config_.width), static_cast<float>(config_.height), 0, 1};
    VkRect2D scissor{{0, 0}, {config_.width, config_.height}};
    
    VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    
    VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    
    VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    
    VkPipelineColorBlendAttachmentState blend{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    
    VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend,
    };
    
    VkPipelineLayoutCreateInfo pl_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout_,
    };
    
    if (vkCreatePipelineLayout(device_, &pl_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create pipeline layout");
    }
    
    VkGraphicsPipelineCreateInfo gp_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .layout = pipeline_layout_,
        .renderPass = render_pass_,
        .subpass = 0,
    };
    
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp_info, nullptr, &pipeline_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create graphics pipeline");
    }
    
    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);
    
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i = 0; i < swapchain_image_views_.size(); ++i) {
        VkFramebufferCreateInfo fb_info{
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass_,
            .attachmentCount = 1,
            .pAttachments = &swapchain_image_views_[i],
            .width = swapchain_extent_.width,
            .height = swapchain_extent_.height,
            .layers = 1,
        };
        if (vkCreateFramebuffer(device_, &fb_info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            return Kaelum::make_unexpected("Failed to create framebuffer");
        }
    }
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_command_pool() {
    VkCommandPoolCreateInfo cp_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_,
    };
    if (vkCreateCommandPool(device_, &cp_info, nullptr, &command_pool_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create command pool");
    }
    
    VkCommandBufferAllocateInfo cb_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    vkAllocateCommandBuffers(device_, &cb_info, command_buffers_);
    
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_sync_objects() {
    image_available_semaphores_.resize(2);
    render_finished_semaphores_.resize(2);
    in_flight_fences_.resize(2);
    
    VkSemaphoreCreateInfo sem_info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    
    for (size_t i = 0; i < 2; ++i) {
        if (vkCreateSemaphore(device_, &sem_info, nullptr, &image_available_semaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sem_info, nullptr, &render_finished_semaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fence_info, nullptr, &in_flight_fences_[i]) != VK_SUCCESS) {
            return Kaelum::make_unexpected("Failed to create sync objects");
        }
    }
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_atlas() {
    VkImageCreateInfo img_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = {1024, 1024, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    
    if (vkCreateImage(device_, &img_info, nullptr, &atlas_image_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create atlas image");
    }
    
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, atlas_image_, &mem_req);
    
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }
    
    if (mem_type == UINT32_MAX) return Kaelum::make_unexpected("Failed to find device local memory");
    
    VkMemoryAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &atlas_memory_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to allocate atlas memory");
    }
    
    vkBindImageMemory(device_, atlas_image_, atlas_memory_, 0);
    
    // Image view
    VkImageViewCreateInfo view_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = atlas_image_,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    
    if (vkCreateImageView(device_, &view_info, nullptr, &atlas_view_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create atlas image view");
    }
    
    // Sampler
    VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    
    if (vkCreateSampler(device_, &sampler_info, nullptr, &atlas_sampler_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create atlas sampler");
    }
    
    return {};
}

Kaelum::Expected<void, std::string> Renderer::create_vertex_buffer() {
    vertex_buffer_capacity_ = 4096 * 4 * (4 + 2 + 4) * sizeof(float); // 4096 cells * 4 vertices * (pos+uv+color)
    
    VkBufferCreateInfo buf_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vertex_buffer_capacity_,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    
    if (vkCreateBuffer(device_, &buf_info, nullptr, &vertex_buffer_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to create vertex buffer");
    }
    
    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, vertex_buffer_, &mem_req);
    
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
    
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            mem_type = i;
            break;
        }
    }
    
    if (mem_type == UINT32_MAX) return Kaelum::make_unexpected("Failed to find host visible memory");
    
    VkMemoryAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &vertex_buffer_memory_) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to allocate vertex buffer memory");
    }
    
    vkBindBufferMemory(device_, vertex_buffer_, vertex_buffer_memory_, 0);
    
    return {};
}

Kaelum::Expected<void, std::string> Renderer::render(const std::vector<DrawCommand>& cmds, const GlyphEngine& glyph_engine) {
    KAELUM_DEBUG("Renderer::render called");
    // Upload atlas if dirty
    if (glyph_engine.atlas_dirty()) {
        upload_atlas(glyph_engine);
    }
    
    // Build vertex buffer
    // ... build vertices from cmds ...
    
    // Render frame
    vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);
    
    uint32_t image_idx;
    VkResult res = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_semaphores_[current_frame_], VK_NULL_HANDLE, &image_idx);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        return Kaelum::make_unexpected("Failed to acquire next image");
    }
    
    KAELUM_DEBUG("Acquired image {}", image_idx);
    
    vkResetCommandBuffer(command_buffers_[current_frame_], 0);
    
    VkCommandBufferBeginInfo begin_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(command_buffers_[current_frame_], &begin_info);
    
    VkClearValue clear{{{0.05f, 0.05f, 0.1f, 1.0f}}};
    VkRenderPassBeginInfo rp_begin{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass_,
        .framebuffer = framebuffers_[image_idx],
        .renderArea = {{0, 0}, swapchain_extent_},
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(command_buffers_[current_frame_], &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    
    vkCmdBindPipeline(command_buffers_[current_frame_], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    
    // Draw commands...
    
    vkCmdEndRenderPass(command_buffers_[current_frame_]);
    vkEndCommandBuffer(command_buffers_[current_frame_]);
    
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &image_available_semaphores_[current_frame_],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffers_[current_frame_],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &render_finished_semaphores_[current_frame_],
    };
    
    if (vkQueueSubmit(queue_, 1, &submit, in_flight_fences_[current_frame_]) != VK_SUCCESS) {
        return Kaelum::make_unexpected("Failed to submit draw commands");
    }
    
    VkPresentInfoKHR present{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished_semaphores_[current_frame_],
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &image_idx,
    };
    
    KAELUM_DEBUG("Presenting image {}", image_idx);
    res = vkQueuePresentKHR(queue_, &present);
    KAELUM_DEBUG("vkQueuePresentKHR returned {}", static_cast<int>(res));
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        // Handle resize
    }
    
    current_frame_ = (current_frame_ + 1) % 2;
    return {};
}

void Renderer::upload_atlas(const GlyphEngine& glyph_engine) {
    // Upload atlas data to GPU
    // Implementation simplified
    glyph_engine.clear_atlas_dirty();
}

Kaelum::Expected<void, std::string> Renderer::resize(uint32_t width, uint32_t height, uint32_t cell_w, uint32_t cell_h) {
    config_.width = width;
    config_.height = height;
    config_.cell_width = cell_w;
    config_.cell_height = cell_h;
    
    // Wait for device to be idle before destroying swapchain
    vkDeviceWaitIdle(device_);
    
    // Recreate swapchain
    cleanup_swapchain();
    auto res = create_swapchain();
    if (!res) return res;
    res = create_framebuffers();
    if (!res) return res;
    return {};
}

} // namespace Kaelum