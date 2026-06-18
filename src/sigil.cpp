#include "sigil.hpp"
#include "wayland-protocols/xdg-shell-client-protocol.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
#include <iostream>
#include <print>
#include <vector>
#include <algorithm>
#include <map>
#include <fstream>
#include <cstring>

#ifdef KAELUM_WITH_LEVEL_ZERO
#include <ze_api.h>
#include <ze_intel_gpu.h>
#endif

namespace Kaelum {

Sigil::Sigil() = default;

Sigil::~Sigil() {
    if (swapchain_ != VK_NULL_HANDLE) {
        cleanup_swapchain();
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }
    if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (graphics_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, graphics_pipeline_, nullptr);
    if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (image_available_semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, image_available_semaphore_, nullptr);
    if (render_finished_semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, render_finished_semaphore_, nullptr);
    if (in_flight_fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, in_flight_fence_, nullptr);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (vk_surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, vk_surface_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    
    if (wl_surface_) wl_surface_destroy(wl_surface_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (display_) wl_display_disconnect(display_);
}


std::expected<void, SigilError> Sigil::initialize() {
    auto w_res = init_wayland();
    if (!w_res) return std::unexpected(w_res.error());

    auto v_res = init_vulkan();
    if (!v_res) return std::unexpected(v_res.error());

    auto s_res = create_swapchain();
    if (!s_res) return std::unexpected(s_res.error());
    create_render_pass();
    create_framebuffers();
    create_graphics_pipeline();
    create_sync_objects();
    create_command_pool();
    record_command_buffers();

    auto l_res = init_level_zero();
    if (!l_res) return std::unexpected(l_res.error());

    std::println("Sigil: Full hardware pipeline initialized successfully.");
    return {};
}

// Wayland Registry Helpers
void registry_handle_global(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    Sigil* sigil = static_cast<Sigil*>(data);
    std::string_view iface(interface);

    if (iface == "wl_compositor") {
        sigil->compositor_ = (struct wl_compositor*)wl_registry_bind(registry, id, &wl_compositor_interface, version);
        std::println("Sigil: Bound wl_compositor");
    } else if (iface == "xdg_wm_base") {
        sigil->xdg_wm_base_ = (struct xdg_wm_base*)wl_registry_bind(registry, id, &xdg_wm_base_interface, version);
        std::println("Sigil: Bound xdg_wm_base");
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = [](void*, struct wl_registry*, uint32_t) {}
};

// XDG Surface Helpers
static void xdg_surface_handle_configure(void* data, struct xdg_surface* xdg_surface, uint32_t serial) {
    Sigil* sigil = static_cast<Sigil*>(data);
    std::println("Sigil: Received xdg_surface configure event (serial: {})", serial);
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_handle_configure,
};

// XDG Toplevel Helpers
static void xdg_toplevel_handle_configure(void* data, struct xdg_toplevel* xdg_toplevel, int32_t width, int32_t height, struct wl_array* states) {
    Sigil* sigil = static_cast<Sigil*>(data);
    if (width > 0 && height > 0) {
        sigil->on_resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_handle_configure,
};

std::expected<void, SigilError> Sigil::init_wayland() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::println(stderr, "Sigil: Failed to connect to Wayland display.");
        return std::unexpected(SigilError::WaylandInitFailed);
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !xdg_wm_base_) {
        std::println(stderr, "Sigil: Failed to bind required Wayland globals.");
        return std::unexpected(SigilError::WaylandInitFailed);
    }

    wl_surface_ = wl_compositor_create_surface(compositor_);
    if (!wl_surface_) return std::unexpected(SigilError::SurfaceCreationFailed);

    xdg_surface_ = xdg_wm_base_get_xdg_surface(xdg_wm_base_, wl_surface_);
    xdg_surface_add_listener(xdg_surface_, &xdg_surface_listener, this);
    
    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_toplevel_add_listener(xdg_toplevel_, &xdg_toplevel_listener, this);

    xdg_toplevel_set_title(xdg_toplevel_, "Kaelum");
    xdg_toplevel_set_app_id(xdg_toplevel_, "org.veridian.kaelum");

    wl_surface_commit(wl_surface_);

    std::println("Sigil: Wayland surface created and mapped.");
    return {};
}

void Sigil::poll_events() {
    while (wl_display_dispatch(display_) != 0) {
        // Keep dispatching until the event queue is empty
    }
}

std::expected<void, SigilError> Sigil::init_vulkan() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Kaelum";
    app_info.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) return std::unexpected(SigilError::VulkanInitFailed);

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    physical_device_ = devices[0];

    // Create Wayland Surface
    VkWaylandSurfaceCreateInfoKHR surface_create_info{};
    surface_create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    surface_create_info.display = display_;
    surface_create_info.surface = wl_surface_;

    if (vkCreateWaylandSurfaceKHR(instance_, &surface_create_info, nullptr, &vk_surface_) != VK_SUCCESS) {
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info{};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = 0; // Simplified
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;
 
    VkDeviceCreateInfo device_create_info{};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;

    // Enable swapchain extension for the device
    std::vector<const char*> device_extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    device_create_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(physical_device_, &device_create_info, nullptr, &device_) != VK_SUCCESS) {
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    vkGetDeviceQueue(device_, 0, 0, &graphics_queue_);

    return {};
}


std::expected<void, SigilError> Sigil::init_level_zero() {
#ifdef KAELUM_WITH_LEVEL_ZERO
    if (zeInit(0) != ZE_RESULT_SUCCESS) {
        return std::unexpected(SigilError::LevelZeroInitFailed);
    }

    uint32_t driver_count = 0;
    zeDriverGet(&driver_count, nullptr);
    if (driver_count > 0) {
        std::vector<ze_driver_handle_t> drivers(driver_count);
        zeDriverGet(&driver_count, drivers.data());
        lz_driver_ = drivers[0];

        uint32_t device_count_lz = 0;
        zeDeviceGet((ze_driver_handle_t)lz_driver_, &device_count_lz, nullptr);
        if (device_count_lz > 0) {
            std::vector<ze_device_handle_t> devices_lz(device_count_lz);
            zeDeviceGet((ze_driver_handle_t)lz_driver_, &device_count_lz, devices_lz.data());
            lz_device_ = devices_lz[0];
        }
    }
#endif
    return {};
}

std::expected<void, SigilError> Sigil::create_swapchain() {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, vk_surface_, &capabilities);
 
    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &format_count, formats.data());
 
    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, vk_surface_, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, vk_surface_, &present_mode_count, present_modes.data());
 
    // Choose format (prefer B8G8R8A8_SRGB)
    VkSurfaceFormatKHR surface_format = formats[0];
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB) {
            surface_format = fmt;
            break;
        }
    }
 
    // Choose present mode (prefer Mailbox for low latency, fall back to FIFO)
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : present_modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = mode;
            break;
        }
    }
 
    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = vk_surface_;
    create_info.minImageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && create_info.minImageCount > capabilities.maxImageCount) {
        create_info.minImageCount = capabilities.maxImageCount;
    }
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent_ = capabilities.currentExtent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
 
    if (vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create swapchain");
        return std::unexpected(SigilError::AllocationFailed);
    }
 
    uint32_t image_count;
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());
 
    swapchain_image_views_.resize(image_count);
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = swapchain_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface_format.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
 
        if (vkCreateImageView(device_, &view_info, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
            std::println(stderr, "Sigil: Failed to create image view for swapchain image {}", i);
        }
    }
    std::println("Sigil: Swapchain created with {} images.", image_count);
    return {};
}


void Sigil::create_framebuffers() {
    swapchain_framebuffers_.resize(swapchain_image_views_.size());

    for (size_t i = 0; i < swapchain_image_views_.size(); ++i) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = render_pass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &swapchain_image_views_[i];
        framebufferInfo.width = extent_.width;
        framebufferInfo.height = extent_.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &swapchain_framebuffers_[i]) != VK_SUCCESS) {
            std::println(stderr, "Sigil: Failed to create framebuffer {}", i);
        }
    }
}

void Sigil::create_render_pass() {

    VkAttachmentDescription color_attachment{};
    color_attachment.format = VK_FORMAT_B8G8R8A8_SRGB;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_attachment_ref{};
    color_attachment_ref.attachment = 0;
    color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment_ref;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;

    if (vkCreateRenderPass(device_, &render_pass_info, nullptr, &render_pass_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create render pass");
    }
}

static VkShaderModule create_shader_module(VkDevice device, const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::println(stderr, "Sigil: Failed to open shader file {}", filename);
        return VK_NULL_HANDLE;
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = fileSize;
    createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create shader module from {}", filename);
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

void Sigil::create_graphics_pipeline() {
    VkShaderModule vertShaderModule = create_shader_module(device_, "shaders/test.vert");
    VkShaderModule fragShaderModule = create_shader_module(device_, "shaders/test.frag");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShaderModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShaderModule;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = 800.0f; 
    viewport.height = 600.0f; 
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {800, 600};

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthClampEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.layout = pipeline_layout_;
    pipelineInfo.renderPass = render_pass_;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphics_pipeline_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create graphics pipeline");
    }

    vkDestroyShaderModule(device_, vertShaderModule, nullptr);
    vkDestroyShaderModule(device_, fragShaderModule, nullptr);
}

void Sigil::cleanup_swapchain() {
    for (auto fb : swapchain_framebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    swapchain_framebuffers_.clear();

    for (auto view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.clear();
    swapchain_images_.clear();
}

void Sigil::create_sync_objects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &image_available_semaphore_) != VK_SUCCESS ||
        vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &render_finished_semaphore_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create semaphores");
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateFence(device_, &fenceInfo, nullptr, &in_flight_fence_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create fence");
    }
}

void Sigil::render(const Nexus& nexus) {
    if (swapchain_ == VK_NULL_HANDLE) return;

    vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &in_flight_fence_);

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_semaphore_, VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        on_resize(0, 0); // Trigger recreation
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        std::println(stderr, "Sigil: Failed to acquire swapchain image");
        return;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {image_available_semaphore_};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command_buffers_[image_index];
    
    VkSemaphore signalSemaphores[] = {render_finished_semaphore_};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphics_queue_, 1, &submitInfo, in_flight_fence_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to submit draw command buffer");
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &image_index;

    if (vkQueuePresentKHR(graphics_queue_, &presentInfo) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to present swapchain image");
    }
}

void Sigil::on_resize(uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(device_);
    cleanup_swapchain();
    if (!create_swapchain()) {
        std::println(stderr, "Sigil: Failed to recreate swapchain during resize");
        return;
    }
    create_framebuffers();
    record_command_buffers();
    
    // In a real implementation, we'd also update viewport and scissor in the pipeline
    std::println("Sigil: Swapchain and framebuffers recreated for size {}x{}", width, height);
}

void Sigil::create_command_pool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = 0;

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &command_pool_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create command pool");
    }
}

void Sigil::record_command_buffers() {
    uint32_t image_count = static_cast<uint32_t>(swapchain_images_.size());
    command_buffers_.resize(image_count);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = command_pool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = image_count;

    if (vkAllocateCommandBuffers(device_, &allocInfo, command_buffers_.data()) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to allocate command buffers");
        return;
    }

    for (uint32_t i = 0; i < image_count; ++i) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(command_buffers_[i], &beginInfo) != VK_SUCCESS) continue;

        VkClearValue clearColor = {{{0.05f, 0.05f, 0.07f, 1.0f}}}; // Dark Nordic background

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = render_pass_;
        renderPassInfo.framebuffer = swapchain_framebuffers_[i];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = extent_;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(command_buffers_[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command_buffers_[i]);
        
        vkEndCommandBuffer(command_buffers_[i]);
    }
}

void Sigil::create_glyph_atlas(GlyphEngine& engine) {
    uint32_t atlas_width = 1024;
    uint32_t atlas_height = 1024;
    
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent = { atlas_width, atlas_height, 1 };
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8_UNORM;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(device_, &image_info, nullptr, &glyph_atlas_image_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create glyph atlas image");
        return;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, glyph_atlas_image_, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = 0; // Simplified

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &glyph_atlas_memory_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to allocate glyph atlas memory");
        return;
    }

    vkBindImageMemory(device_, glyph_atlas_image_, glyph_atlas_memory_, 0);

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = glyph_atlas_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_, &view_info, nullptr, &glyph_atlas_view_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create glyph atlas view");
        return;
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device_, &sampler_info, nullptr, &glyph_atlas_sampler_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create glyph atlas sampler");
        return;
    }

    // Now pack the glyphs
    std::vector<uint8_t> atlas_data(atlas_width * atlas_height, 0);
    uint32_t current_x = 0;
    uint32_t current_y = 0;
    uint32_t max_row_height = 0;

    // For now, we'll just load ASCII 32-126
    for (char32_t c = 32; c < 127; ++c) {
        auto glyph_res = engine.get_glyph(c);
        if (!glyph_res) continue;

        const auto& glyph = *glyph_res;
        if (current_x + glyph.metric.width > atlas_width) {
            current_x = 0;
            current_y += max_row_height;
            max_row_height = 0;
        }

        if (current_y + glyph.metric.height > atlas_height) {
            std::println(stderr, "Sigil: Glyph atlas overflow!");
            break;
        }

        for (uint32_t row = 0; row < glyph.metric.height; ++row) {
            std::memcpy(&atlas_data[(current_y + row) * atlas_width + current_x], 
                        &glyph.bitmap[row * glyph.metric.width], 
                        glyph.metric.width);
        }

        current_x += glyph.metric.width;
        max_row_height = std::max(max_row_height, glyph.metric.height);
    }

    // Transfer atlas_data to GPU
    VkBuffer staging_buffer;
    VkDeviceMemory staging_buffer_memory;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = atlas_data.size();
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buffer_info, nullptr, &staging_buffer) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create staging buffer");
        return;
    }

    VkMemoryRequirements mem_reqs_buf;
    vkGetBufferMemoryRequirements(device_, staging_buffer, &mem_reqs_buf);

    VkMemoryAllocateInfo alloc_info_buf{};
    alloc_info_buf.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info_buf.allocationSize = mem_reqs_buf.size;
    alloc_info_buf.memoryTypeIndex = 0; // Simplified

    if (vkAllocateMemory(device_, &alloc_info_buf, nullptr, &staging_buffer_memory) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to allocate staging buffer memory");
        return;
    }

    vkBindBufferMemory(device_, staging_buffer, staging_buffer_memory, 0);

    void* data;
    vkMapMemory(device_, staging_buffer_memory, 0, atlas_data.size(), 0, &data);
    std::memcpy(data, atlas_data.data(), atlas_data.size());
    vkUnmapMemory(device_, staging_buffer_memory);

    VkCommandBufferAllocateInfo cb_alloc_info{};
    cb_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc_info.commandPool = command_pool_;
    cb_alloc_info.commandBufferCount = 1;

    VkCommandBuffer upload_cb;
    vkAllocateCommandBuffers(device_, &cb_alloc_info, &upload_cb);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(upload_cb, &begin_info);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = glyph_atlas_image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(upload_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { atlas_width, atlas_height, 1 };

    vkCmdCopyBufferToImage(upload_cb, staging_buffer, glyph_atlas_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(upload_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(upload_cb);

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &upload_cb;

    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &upload_cb);
    vkDestroyBuffer(device_, staging_buffer, nullptr);
    vkFreeMemory(device_, staging_buffer_memory, nullptr);

    std::println("Sigil: Glyph atlas uploaded to GPU successfully.");
}

void Sigil::initialize_assets(GlyphEngine& engine) {
    create_glyph_atlas(engine);
}

} // namespace Kaelum

