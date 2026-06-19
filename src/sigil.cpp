#include "sigil.hpp"
#include "wayland-protocols/xdg-shell-client-protocol.hpp"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
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
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);

    if (swapchain_ != VK_NULL_HANDLE) {
        cleanup_swapchain();
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    }

    // Glyph atlas resources
    if (glyph_atlas_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, glyph_atlas_sampler_, nullptr);
    if (glyph_atlas_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, glyph_atlas_view_, nullptr);
    if (glyph_atlas_image_ != VK_NULL_HANDLE) vkDestroyImage(device_, glyph_atlas_image_, nullptr);
    if (glyph_atlas_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, glyph_atlas_memory_, nullptr);

    // Vertex buffer resources
    if (vertex_buffer_mapped_) vkUnmapMemory(device_, vertex_buffer_memory_);
    if (vertex_buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
    if (vertex_buffer_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, vertex_buffer_memory_, nullptr);

    // Descriptor resources
    if (descriptor_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);

    if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (graphics_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, graphics_pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (image_available_semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, image_available_semaphore_, nullptr);
    if (render_finished_semaphore_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, render_finished_semaphore_, nullptr);
    if (in_flight_fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, in_flight_fence_, nullptr);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (vk_surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, vk_surface_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);

    // Wayland cleanup
    if (frame_callback_) wl_callback_destroy(frame_callback_);
    if (pointer_) wl_pointer_destroy(pointer_);
    if (keyboard_) wl_keyboard_destroy(keyboard_);
    if (xdg_toplevel_) xdg_toplevel_destroy(xdg_toplevel_);
    if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
    if (xdg_wm_base_) xdg_wm_base_destroy(xdg_wm_base_);
    if (wl_surface_) wl_surface_destroy(wl_surface_);
    if (seat_) wl_seat_destroy(seat_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
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
    create_sync_objects();
    create_command_pool();
    create_graphics_pipeline();
    record_command_buffers();
    create_vertex_buffer();

    auto l_res = init_level_zero();
    if (!l_res) return std::unexpected(l_res.error());

    initial_configure_done_ = true;
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
        static const struct xdg_wm_base_listener wm_base_listener = {
            .ping = [](void*, struct xdg_wm_base* wm_base, uint32_t serial) {
                xdg_wm_base_pong(wm_base, serial);
            }
        };
        xdg_wm_base_add_listener(sigil->xdg_wm_base_, &wm_base_listener, sigil);
        std::println("Sigil: Bound xdg_wm_base");
    } else if (iface == "wl_seat") {
        sigil->seat_ = (struct wl_seat*)wl_registry_bind(registry, id, &wl_seat_interface, std::min(version, 5u));
        std::println("Sigil: Bound wl_seat");
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = [](void*, struct wl_registry*, uint32_t) {}
};

// XDG Surface Helpers
static void xdg_surface_handle_configure(void* data, struct xdg_surface* xdg_surface, uint32_t serial) {
    Sigil* sigil = static_cast<Sigil*>(data);
    xdg_surface_ack_configure(xdg_surface, serial);
    // Per Wayland protocol: ack → set geometry → commit
    uint32_t w = sigil->configured_width();
    uint32_t h = sigil->configured_height();
    if (w > 0 && h > 0) {
        xdg_surface_set_window_geometry(xdg_surface, 0, 0, w, h);
    }
    if (sigil->get_wl_surface()) {
        wl_surface_commit(sigil->get_wl_surface());
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_handle_configure,
};

// XDG Toplevel Helpers
static void xdg_toplevel_handle_configure(void* data, struct xdg_toplevel* /*xdg_toplevel*/, int32_t width, int32_t height, struct wl_array* /*states*/) {
    Sigil* sigil = static_cast<Sigil*>(data);
    // width/height == 0 means compositor lets us choose; keep current configured size
    if (width > 0 && height > 0) {
        sigil->handle_configure(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
}

static void xdg_toplevel_handle_close(void* data, struct xdg_toplevel* /*xdg_toplevel*/) {
    Sigil* sigil = static_cast<Sigil*>(data);
    sigil->request_close();
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_handle_configure,
    .close = xdg_toplevel_handle_close,
    .configure_bounds = [](void*, struct xdg_toplevel*, int32_t, int32_t) {},
    .wm_capabilities = [](void*, struct xdg_toplevel*, struct wl_array*) {},
};

// Pointer listener — handles cursor enter/leave for proper Hyprland focus
static const struct wl_pointer_listener pointer_listener_inst = {
    .enter = [](void*, struct wl_pointer* pointer, uint32_t serial,
               struct wl_surface*, wl_fixed_t, wl_fixed_t) {
        wl_pointer_set_cursor(pointer, serial, nullptr, 0, 0);
    },
    .leave = [](void*, struct wl_pointer*, uint32_t, struct wl_surface*) {},
    .motion = [](void*, struct wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {},
    .button = [](void*, struct wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {},
    .axis = [](void*, struct wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
    .frame = [](void*, struct wl_pointer*) {},
    .axis_source = [](void*, struct wl_pointer*, uint32_t) {},
    .axis_stop = [](void*, struct wl_pointer*, uint32_t, uint32_t) {},
    .axis_discrete = [](void*, struct wl_pointer*, uint32_t, int32_t) {},
};

static void keyboard_handle_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)keyboard;
    (void)serial;
    (void)time;
    Sigil* sigil = static_cast<Sigil*>(data);
    sigil->handle_key_event(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = [](void*, struct wl_keyboard*, uint32_t, int32_t, uint32_t) {},
    .enter = [](void*, struct wl_keyboard*, uint32_t, struct wl_surface*, struct wl_array*) {},
    .leave = [](void*, struct wl_keyboard*, uint32_t, struct wl_surface*) {},
    .key = keyboard_handle_key,
    .modifiers = [](void*, struct wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
    .repeat_info = [](void*, struct wl_keyboard*, int32_t, int32_t) {},
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
    xdg_toplevel_set_min_size(xdg_toplevel_, 320, 240);

    if (seat_) {
        keyboard_ = wl_seat_get_keyboard(seat_);
        wl_keyboard_add_listener(keyboard_, &keyboard_listener, this);

        pointer_ = wl_seat_get_pointer(seat_);
        if (pointer_) {
            wl_pointer_add_listener(pointer_, &pointer_listener_inst, this);
        }
        std::println("Sigil: Keyboard listener attached.");
    }

    wl_surface_commit(wl_surface_);
    wl_display_roundtrip(display_);

    std::println("Sigil: Wayland surface created and mapped.");
    return {};
}

void Sigil::set_keyboard_callback(std::function<void(uint32_t key, bool pressed)> callback) {
    keyboard_callback_ = callback;
}

void Sigil::handle_key_event(uint32_t key, bool pressed) {
    if (keyboard_callback_) {
        keyboard_callback_(key, pressed);
    }
}

void Sigil::poll_events() {
    wl_display_read_events(display_);
    wl_display_dispatch_pending(display_);
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
        std::println(stderr, "Sigil: Failed to create Vulkan instance.");
        return std::unexpected(SigilError::VulkanInitFailed);
    }


    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) return std::unexpected(SigilError::VulkanInitFailed);

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    physical_device_ = devices[0];

    VkWaylandSurfaceCreateInfoKHR surface_create_info{};
    surface_create_info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    surface_create_info.display = display_;
    surface_create_info.surface = wl_surface_;

    if (vkCreateWaylandSurfaceKHR(instance_, &surface_create_info, nullptr, &vk_surface_) != VK_SUCCESS) {
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    // Find a queue family that supports both graphics and presentation
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());

    graphics_queue_family_ = UINT32_MAX;
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, vk_surface_, &present_support);
        if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
            graphics_queue_family_ = i;
            break;
        }
    }
    if (graphics_queue_family_ == UINT32_MAX) {
        std::println(stderr, "Sigil: No queue family supports both graphics and presentation.");
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info{};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_;
    queue_create_info.queueCount = 1;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo device_create_info{};
    device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_create_info.queueCreateInfoCount = 1;
    device_create_info.pQueueCreateInfos = &queue_create_info;

    std::vector<const char*> device_extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    device_create_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(physical_device_, &device_create_info, nullptr, &device_) != VK_SUCCESS) {
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);

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

std::expected<void, SigilError> Sigil::create_swapchain(VkSwapchainKHR old_swapchain) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, vk_surface_, &capabilities);

    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &format_count, nullptr);
    if (format_count == 0) {
        std::println(stderr, "Sigil: No surface formats available.");
        return std::unexpected(SigilError::AllocationFailed);
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, vk_surface_, &format_count, formats.data());
 
    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, vk_surface_, &present_mode_count, nullptr);
    if (present_mode_count == 0) {
        std::println(stderr, "Sigil: No present modes available.");
        return std::unexpected(SigilError::AllocationFailed);
    }
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
    swapchain_format_ = surface_format.format;
 
    // Choose present mode (prefer Mailbox for low latency, fall back to FIFO)
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : present_modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = mode;
            break;
        }
    }
 
    VkExtent2D actualExtent = capabilities.currentExtent;
    if (actualExtent.width == UINT32_MAX || actualExtent.width == 0) {
        actualExtent.width = configured_width_;
        actualExtent.height = configured_height_;
    }
    if (capabilities.maxImageExtent.width > 0) {
        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    if (actualExtent.width == 0 || actualExtent.height == 0) {
        return std::unexpected(SigilError::AllocationFailed);
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
    create_info.imageExtent = extent_ = actualExtent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = capabilities.currentTransform;

    VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto flag : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
        if (capabilities.supportedCompositeAlpha & flag) {
            composite_alpha = flag;
            break;
        }
    }
    create_info.compositeAlpha = composite_alpha;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = old_swapchain;
 
    VkResult sc_result = vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_);
    if (sc_result != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create swapchain (VkResult: {}, extent: {}x{}, format: {}, composite: {})",
                     static_cast<int>(sc_result), actualExtent.width, actualExtent.height,
                     static_cast<int>(surface_format.format), static_cast<int>(composite_alpha));
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
    color_attachment.format = swapchain_format_;
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
    VkShaderModule vertShaderModule = create_shader_module(device_, "shaders/test.vert.spv");
    VkShaderModule fragShaderModule = create_shader_module(device_, "shaders/test.frag.spv");

    if (vertShaderModule == VK_NULL_HANDLE || fragShaderModule == VK_NULL_HANDLE) {
        std::println(stderr, "Sigil: Failed to load one or more shaders");
        return;
    }
    
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShaderModule;
    stages[0].pName = "main";
    
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShaderModule;
    stages[1].pName = "main";
    
    VkVertexInputBindingDescription binding_desc{};
    binding_desc.binding = 0;
    binding_desc.stride = sizeof(SigilVertex);
    binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attr_desc[3] = {};
    attr_desc[0].location = 0;
    attr_desc[0].binding = 0;
    attr_desc[0].format = VK_FORMAT_R32G32_SFLOAT;
    attr_desc[0].offset = offsetof(SigilVertex, pos);

    attr_desc[1].location = 1;
    attr_desc[1].binding = 0;
    attr_desc[1].format = VK_FORMAT_R32G32_SFLOAT;
    attr_desc[1].offset = offsetof(SigilVertex, uv);

    attr_desc[2].location = 2;
    attr_desc[2].binding = 0;
    attr_desc[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attr_desc[2].offset = offsetof(SigilVertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &binding_desc;
    vertexInputInfo.vertexAttributeDescriptionCount = 3;
    vertexInputInfo.pVertexAttributeDescriptions = attr_desc;
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)extent_.width; 
    viewport.height = (float)extent_.height; 
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent_;
    
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
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    
    VkDescriptorSetLayoutBinding layout_binding{};
    layout_binding.binding = 0;
    layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layout_binding.descriptorCount = 1;
    layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &layout_binding;

    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create descriptor set layout");
        return;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptor_set_layout_;

    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create pipeline layout");
        return;
    }
    
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    dynamicState.pDynamicStates = dynamicStates;
    
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
    pipelineInfo.pDynamicState = &dynamicState;
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

static void frame_done_cb(void* data, struct wl_callback* callback, uint32_t /*time*/) {
    wl_callback_destroy(callback);
    static_cast<Sigil*>(data)->frame_done();
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done_cb,
};

void Sigil::render(const Nexus& nexus) {
    if (swapchain_ == VK_NULL_HANDLE) return;
    if (frame_pending_) return;

    vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_semaphore_, VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        on_resize(0, 0);
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        std::println(stderr, "Sigil: Failed to acquire swapchain image");
        return;
    }

    vkResetFences(device_, 1, &in_flight_fence_);

    const auto& grid = nexus.get_grid();
    size_t n_cols = nexus.cols();
    size_t n_rows = nexus.rows();
    std::vector<SigilVertex> vertices;

    // Convert pixel dimensions to NDC units
    float px_to_ndc_x = 2.0f / extent_.width;
    float px_to_ndc_y = 2.0f / extent_.height;
    float cell_w_ndc = cell_width_ * px_to_ndc_x;
    float cell_h_ndc = cell_height_ * px_to_ndc_y;

    for (uint32_t y = 0; y < n_rows; ++y) {
        for (uint32_t x = 0; x < n_cols; ++x) {
            const auto& cell = grid[y * n_cols + x];
            if (cell.codepoint == U' ') continue;

            auto it = glyph_map_.find(cell.codepoint);
            if (it == glyph_map_.end()) continue;

            const auto& gr = it->second;

            // Cell origin in NDC (top-left of cell)
            float cell_x = -1.0f + x * cell_w_ndc;
            float cell_y = 1.0f - y * cell_h_ndc;

            // Position glyph within cell using bearing offsets
            float glyph_x0 = cell_x + gr.bearing_x * px_to_ndc_x;
            float glyph_y0 = cell_y - gr.bearing_y * px_to_ndc_y;
            float glyph_x1 = glyph_x0 + gr.width * px_to_ndc_x;
            float glyph_y1 = glyph_y0 + gr.height * px_to_ndc_y;

            float r = cell.fg.r / 255.0f, g = cell.fg.g / 255.0f;
            float b = cell.fg.b / 255.0f, a = cell.fg.a / 255.0f;

            // Two triangles for the glyph quad (top-left origin, Y-down in NDC)
            vertices.push_back({{glyph_x0, glyph_y1}, {gr.u0, gr.v1}, {r, g, b, a}});
            vertices.push_back({{glyph_x1, glyph_y1}, {gr.u1, gr.v1}, {r, g, b, a}});
            vertices.push_back({{glyph_x0, glyph_y0}, {gr.u0, gr.v0}, {r, g, b, a}});
            vertices.push_back({{glyph_x1, glyph_y1}, {gr.u1, gr.v1}, {r, g, b, a}});
            vertices.push_back({{glyph_x1, glyph_y0}, {gr.u1, gr.v0}, {r, g, b, a}});
            vertices.push_back({{glyph_x0, glyph_y0}, {gr.u0, gr.v0}, {r, g, b, a}});
        }
    }

    if (vertices.size() > vertex_buffer_capacity_) {
        vertices.resize(vertex_buffer_capacity_);
    }
    if (!vertices.empty()) {
        std::memcpy(vertex_buffer_mapped_, vertices.data(), vertices.size() * sizeof(SigilVertex));
    }

    // --- Submit Render Command ---
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {image_available_semaphore_};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    // We need to record a NEW command buffer for this frame because we're drawing dynamic vertices
    // To keep it simple, we'll just use the one for the image index and re-record it.
    VkCommandBuffer cb = command_buffers_[image_index];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &beginInfo);

    // Veridian Zenith: deep void background (#050200)
    VkClearValue clearColor = {{{5.0f/255.0f, 2.0f/255.0f, 0.0f/255.0f, 1.0f}}};
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = render_pass_;
    renderPassInfo.framebuffer = swapchain_framebuffers_[image_index];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent_;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cb, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)extent_.width;
    viewport.height = (float)extent_.height;
    vkCmdSetViewport(cb, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent_;
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cb, 0, 1, &vertex_buffer_, offsets);
    vkCmdDraw(cb, static_cast<uint32_t>(vertices.size()), 1, 0, 0);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cb;
    
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

    VkResult present_result = vkQueuePresentKHR(graphics_queue_, &presentInfo);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        on_resize(0, 0);
    } else if (present_result != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to present swapchain image");
    }

    // Request frame callback for vsync pacing
    frame_callback_ = wl_surface_frame(wl_surface_);
    wl_callback_add_listener(frame_callback_, &frame_listener, this);
    wl_surface_commit(wl_surface_);
    frame_pending_ = true;
}

void Sigil::handle_configure(uint32_t width, uint32_t height) {
    if (width > 0 && height > 0) {
        if (width != configured_width_ || height != configured_height_) {
            configured_width_ = width;
            configured_height_ = height;
            if (initial_configure_done_) {
                needs_resize_ = true;
            }
        }
    }
}

void Sigil::process_pending_resize() {
    if (!needs_resize_) return;
    needs_resize_ = false;
    on_resize(configured_width_, configured_height_);
}

void Sigil::on_resize(uint32_t width, uint32_t height) {
    if (device_ == VK_NULL_HANDLE) return;
    if (width > 0 && height > 0) {
        configured_width_ = width;
        configured_height_ = height;
    }
    vkDeviceWaitIdle(device_);
    cleanup_swapchain();

    VkSwapchainKHR old_swapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;

    if (!create_swapchain(old_swapchain)) {
        std::println(stderr, "Sigil: Failed to recreate swapchain during resize");
        if (old_swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
        return;
    }
    if (old_swapchain != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device_, old_swapchain, nullptr);

    create_framebuffers();
    record_command_buffers();

    if (resize_callback_ && cell_width_ > 0 && cell_height_ > 0) {
        uint32_t cols = extent_.width / cell_width_;
        uint32_t rows = extent_.height / cell_height_;
        if (cols > 0 && rows > 0)
            resize_callback_(cols, rows, extent_.width, extent_.height);
    }
}

void Sigil::create_command_pool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphics_queue_family_;

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &command_pool_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create command pool");
    }
}

void Sigil::record_command_buffers() {
    if (!command_buffers_.empty()) {
        vkFreeCommandBuffers(device_, command_pool_,
            static_cast<uint32_t>(command_buffers_.size()), command_buffers_.data());
        command_buffers_.clear();
    }

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

        VkClearValue clearColor = {{{5.0f/255.0f, 2.0f/255.0f, 0.0f/255.0f, 1.0f}}};

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
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
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

    std::vector<uint8_t> atlas_data(atlas_width * atlas_height, 0);
    uint32_t current_x = 0;
    uint32_t current_y = 0;
    uint32_t max_row_height = 0;

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

        glyph_map_[c] = {
            (float)current_x / atlas_width,
            (float)current_y / atlas_height,
            (float)(current_x + glyph.metric.width) / atlas_width,
            (float)(current_y + glyph.metric.height) / atlas_height,
            static_cast<int32_t>(glyph.metric.bearing_x),
            static_cast<int32_t>(glyph.metric.bearing_y),
            glyph.metric.width,
            glyph.metric.height,
            glyph.metric.advance
        };

        current_x += glyph.metric.width;
        max_row_height = std::max(max_row_height, glyph.metric.height);
    }

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
    alloc_info_buf.memoryTypeIndex = find_memory_type(mem_reqs_buf.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
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
    create_descriptor_set();
}

uint32_t Sigil::find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

void Sigil::create_vertex_buffer() {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertex_buffer_capacity_ = 20000 * 6;
    buffer_info.size = vertex_buffer_capacity_ * sizeof(SigilVertex);
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &buffer_info, nullptr, &vertex_buffer_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create vertex buffer");
        return;
    }

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device_, vertex_buffer_, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &vertex_buffer_memory_) != VK_SUCCESS) {

        std::println(stderr, "Sigil: Failed to allocate vertex buffer memory");
        return;
    }

    vkBindBufferMemory(device_, vertex_buffer_, vertex_buffer_memory_, 0);
    vkMapMemory(device_, vertex_buffer_memory_, 0, 10000 * 6 * sizeof(SigilVertex), 0, &vertex_buffer_mapped_);
}

void Sigil::create_descriptor_set() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create descriptor pool");
        return;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_set_layout_;

    if (vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to allocate descriptor set");
        return;
    }

    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = glyph_atlas_view_;
    image_info.sampler = glyph_atlas_sampler_;

    VkWriteDescriptorSet descriptor_write{};
    descriptor_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_write.dstSet = descriptor_set_;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.descriptorCount = 1;
    descriptor_write.pImageInfo = &image_info;

    vkUpdateDescriptorSets(device_, 1, &descriptor_write, 0, nullptr);
}
} // namespace Kaelum


