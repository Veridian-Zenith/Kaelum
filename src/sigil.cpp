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
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>

#ifdef KAELUM_WITH_LEVEL_ZERO
#include <ze_api.h>
#include <ze_intel_gpu.h>
#endif

namespace Kaelum {

// --- Wayland listener trampolines ---

void Sigil::wl_registry_global(void* data, wl_registry* reg, uint32_t name,
                                const char* iface, uint32_t version) {
    auto* self = static_cast<Sigil*>(data);
    std::println(stdout, "Wayland global: {} v{}", iface, version);

    if (std::strcmp(iface, "wl_compositor") == 0) {
        self->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, "xdg_wm_base") == 0) {
        self->xdg_wm_base_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 7));
        xdg_wm_base_add_listener(self->xdg_wm_base_, &self->xdg_wm_listener_, data);
    } else if (std::strcmp(iface, "wl_seat") == 0) {
        self->seat_ = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, 7));
        wl_seat_add_listener(self->seat_, &self->seat_listener_, data);
    } else if (std::strcmp(iface, "wl_output") == 0) {
        // Just take the first output
        if (!self->output_) {
            self->output_ = static_cast<wl_output*>(
                wl_registry_bind(reg, name, &wl_output_interface, 1));
        }
    }
}

void Sigil::wl_registry_global_remove(void* data, wl_registry* reg, uint32_t name) {}

void Sigil::xdg_surface_configure(void* data, xdg_surface* surf, uint32_t serial) {
    auto* self = static_cast<Sigil*>(data);
    self->configured_ = true;
    xdg_surface_ack_configure(surf, serial);
}

void Sigil::xdg_toplevel_configure(void* data, xdg_toplevel* top,
                                    int32_t w, int32_t h, wl_array* states) {
    auto* self = static_cast<Sigil*>(data);
    if (w > 0 && h > 0) {
        self->width_ = static_cast<uint32_t>(w);
        self->height_ = static_cast<uint32_t>(h);
    }
}

void Sigil::xdg_toplevel_close(void* data, xdg_toplevel* top) {
    auto* self = static_cast<Sigil*>(data);
    self->closing_ = true;
}

void Sigil::xdg_toplevel_configure_bounds(void* data, xdg_toplevel* top,
                                           int32_t w, int32_t h) {}

void Sigil::xdg_toplevel_wm_capabilities(void* data, xdg_toplevel* top,
                                          wl_array* caps) {}

void Sigil::xdg_wm_base_ping(void* data, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

// --- Keyboard input handling ---

void Sigil::wl_keyboard_keymap(void* data, wl_keyboard* kb, uint32_t fmt,
                                int fd, uint32_t size) {
    auto* self = static_cast<Sigil*>(data);
    auto& xkb = self->xkb_state_;

    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    if (!xkb.ctx) {
        xkb.ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    }

    char* map_str = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    if (xkb.keymap) xkb_keymap_unref(xkb.keymap);
    xkb.keymap = xkb_keymap_new_from_string(xkb.ctx, map_str,
                                             XKB_KEYMAP_FORMAT_TEXT_V1,
                                             static_cast<xkb_keymap_compile_flags>(0));
    munmap(map_str, size);
    close(fd);

    if (!xkb.keymap) return;

    if (xkb.state) xkb_state_unref(xkb.state);
    xkb.state = xkb_state_new(xkb.keymap);

    // Cache modifier indices
    xkb.alt_mod = xkb_keymap_mod_get_index(xkb.keymap, XKB_MOD_NAME_ALT);
    xkb.ctrl_mod = xkb_keymap_mod_get_index(xkb.keymap, XKB_MOD_NAME_CTRL);
}

void Sigil::wl_keyboard_enter(void* data, wl_keyboard* kb, uint32_t serial,
                               wl_surface* surf, wl_array* keys) {}

void Sigil::wl_keyboard_leave(void* data, wl_keyboard* kb, uint32_t serial,
                               wl_surface* surf) {}

void Sigil::wl_keyboard_key(void* data, wl_keyboard* kb, uint32_t serial,
                             uint32_t time, uint32_t key, uint32_t kstate) {
    auto* self = static_cast<Sigil*>(data);
    if (kstate != WL_KEYBOARD_KEY_STATE_PRESSED) return;

    auto& xkb = self->xkb_state_;
    if (!xkb.state || !xkb.keymap) return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb.state, key + 8);

    char buf[64] = {};
    int len = xkb_state_key_get_utf8(xkb.state, key + 8, buf, sizeof(buf));

    if (len > 0) {
        self->keyboard_cb_(std::string(buf, static_cast<size_t>(len)));
    } else {
        std::string seq;
        switch (sym) {
        case XKB_KEY_Return:     seq = "\r"; break;
        case XKB_KEY_BackSpace:  seq = "\x7F"; break;
        case XKB_KEY_Tab:        seq = "\t"; break;
        case XKB_KEY_Escape:     seq = "\x1B"; break;
        case XKB_KEY_Delete:     seq = "\x1B[3~"; break;
        case XKB_KEY_Home:       seq = "\x1B[H"; break;
        case XKB_KEY_End:        seq = "\x1B[F"; break;
        case XKB_KEY_Up:         seq = "\x1B[A"; break;
        case XKB_KEY_Down:       seq = "\x1B[B"; break;
        case XKB_KEY_Left:       seq = "\x1B[D"; break;
        case XKB_KEY_Right:      seq = "\x1B[C"; break;
        case XKB_KEY_Page_Up:    seq = "\x1B[5~"; break;
        case XKB_KEY_Page_Down:  seq = "\x1B[6~"; break;
        case XKB_KEY_Insert:     seq = "\x1B[2~"; break;
        default: break;
        }

        if (!seq.empty()) {
            xkb_mod_mask_t mods = xkb_state_serialize_mods(xkb.state, XKB_STATE_MODS_DEPRESSED);
            if (xkb.alt_mod != XKB_MOD_INVALID && (mods & (1 << xkb.alt_mod))) {
                seq = "\x1B" + seq;
            }
            self->keyboard_cb_(seq);
        }
    }
}

void Sigil::wl_keyboard_modifiers(void* data, wl_keyboard* kb,
                                   uint32_t serial, uint32_t mods_depressed,
                                   uint32_t mods_latched, uint32_t mods_locked,
                                   uint32_t group) {
    auto* self = static_cast<Sigil*>(data);
    auto& xkb = self->xkb_state_;
    if (xkb.state) {
        xkb_state_update_mask(xkb.state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);
    }
}

void Sigil::wl_keyboard_repeat_info(void* data, wl_keyboard* kb,
                                     int32_t rate, int32_t delay) {}

void Sigil::wl_seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* self = static_cast<Sigil*>(data);
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!self->keyboard_) {
            self->keyboard_ = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(self->keyboard_, &self->kb_listener_, data);
        }
    } else {
        if (self->keyboard_) {
            wl_keyboard_destroy(self->keyboard_);
            self->keyboard_ = nullptr;
        }
    }
}

void Sigil::wl_seat_name(void* data, wl_seat* seat, const char* name) {}

// --- Sigil implementation ---

Sigil::Sigil() {
    // Initialize listener structs
    xdg_wm_listener_ = {.ping = xdg_wm_base_ping};
    xdg_surf_listener_ = {.configure = xdg_surface_configure};
    xdg_top_listener_ = {.configure = xdg_toplevel_configure,
                         .close = xdg_toplevel_close,
                         .configure_bounds = xdg_toplevel_configure_bounds,
                         .wm_capabilities = xdg_toplevel_wm_capabilities};
    kb_listener_ = {.keymap = wl_keyboard_keymap,
                    .enter = wl_keyboard_enter,
                    .leave = wl_keyboard_leave,
                    .key = wl_keyboard_key,
                    .modifiers = wl_keyboard_modifiers,
                    .repeat_info = wl_keyboard_repeat_info};
    seat_listener_ = {.capabilities = wl_seat_capabilities,
                      .name = wl_seat_name};
}

Sigil::~Sigil() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (in_flight_fences_[i] != VK_NULL_HANDLE) vkDestroyFence(device_, in_flight_fences_[i], nullptr);
        if (image_available_sems_[i] != VK_NULL_HANDLE) vkDestroySemaphore(device_, image_available_sems_[i], nullptr);
        if (render_finished_sems_[i] != VK_NULL_HANDLE) vkDestroySemaphore(device_, render_finished_sems_[i], nullptr);
    }

    if (command_pool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(device_, command_pool_, nullptr);

    if (descriptor_pool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);

    if (atlas_sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, atlas_sampler_, nullptr);
    if (atlas_image_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, atlas_image_view_, nullptr);
    if (atlas_image_ != VK_NULL_HANDLE) vkDestroyImage(device_, atlas_image_, nullptr);
    if (atlas_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, atlas_memory_, nullptr);

    if (vertex_buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
    if (vertex_buffer_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, vertex_buffer_memory_, nullptr);

    cleanup_swapchain();

    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (surface_khr_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_khr_, nullptr);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);

    // Clean up Wayland
    if (keyboard_) wl_keyboard_destroy(keyboard_);
    if (seat_) wl_seat_destroy(seat_);
    if (xdg_toplevel_) xdg_toplevel_destroy(xdg_toplevel_);
    if (xdg_surface_) xdg_surface_destroy(xdg_surface_);
    if (xdg_wm_base_) xdg_wm_base_destroy(xdg_wm_base_);
    if (surface_) wl_surface_destroy(surface_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
}

std::expected<void, SigilError> Sigil::init() {
    auto wayland_res = init_wayland();
    if (!wayland_res) return wayland_res;

    auto vulkan_res = init_vulkan();
    if (!vulkan_res) return vulkan_res;

    auto swapchain_res = init_swapchain();
    if (!swapchain_res) return swapchain_res;

    auto rp_res = init_render_pass();
    if (!rp_res) return rp_res;

    auto desc_res = init_descriptors();
    if (!desc_res) return desc_res;

    auto pipe_res = init_pipeline();
    if (!pipe_res) return pipe_res;

    auto fb_res = init_framebuffers();
    if (!fb_res) return fb_res;

    auto cp_res = init_command_pool();
    if (!cp_res) return cp_res;

    auto sync_res = init_sync_objects();
    if (!sync_res) return sync_res;

    auto at_res = init_atlas_texture();
    if (!at_res) return at_res;

    auto vb_res = init_vertex_buffer();
    if (!vb_res) return vb_res;

    return {};
}

std::expected<void, SigilError> Sigil::init_wayland() {
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        std::println(stderr, "Sigil: Failed to connect to Wayland display");
        return std::unexpected(SigilError::WaylandInitFailed);
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_listener registry_listener = {
        .global = wl_registry_global,
        .global_remove = wl_registry_global_remove,
    };
    wl_registry_add_listener(registry_, &registry_listener, this);

    wl_display_roundtrip(display_);

    if (!compositor_ || !xdg_wm_base_) {
        std::println(stderr, "Sigil: Missing required Wayland globals");
        return std::unexpected(SigilError::WaylandInitFailed);
    }

    surface_ = wl_compositor_create_surface(compositor_);
    xdg_surface_ = xdg_wm_base_get_xdg_surface(xdg_wm_base_, surface_);
    xdg_surface_add_listener(xdg_surface_, &xdg_surf_listener_, this);

    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    xdg_toplevel_add_listener(xdg_toplevel_, &xdg_top_listener_, this);

    xdg_toplevel_set_title(xdg_toplevel_, "Kaelum");
    xdg_toplevel_set_app_id(xdg_toplevel_, "io.github.veridian-zenith.kaelum");

    wl_surface_commit(surface_);
    wl_display_roundtrip(display_);

    return {};
}

std::expected<void, SigilError> Sigil::init_vulkan() {
    // Create instance
    std::vector<const char*> extensions = {
        "VK_KHR_surface",
        "VK_KHR_wayland_surface",
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    std::vector<const char*> layers;
#ifdef KAELUM_DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    VkApplicationInfo app_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Kaelum",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "Kaelum",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };

    VkInstanceCreateInfo inst_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledLayerCount = static_cast<uint32_t>(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    if (vkCreateInstance(&inst_info, nullptr, &instance_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create Vulkan instance");
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    // Create Wayland surface
    VkWaylandSurfaceCreateInfoKHR surface_info{
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = display_,
        .surface = surface_,
    };

    auto vkCreateWaylandSurface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
        vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
    if (!vkCreateWaylandSurface ||
        vkCreateWaylandSurface(instance_, &surface_info, nullptr, &surface_khr_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create Wayland surface");
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    // Pick physical device
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance_, &dev_count, nullptr);
    if (dev_count == 0) {
        std::println(stderr, "Sigil: No Vulkan-capable devices");
        return std::unexpected(SigilError::DeviceInitFailed);
    }

    std::vector<VkPhysicalDevice> devices(dev_count);
    vkEnumeratePhysicalDevices(instance_, &dev_count, devices.data());

    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        uint32_t qfam_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qfam_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfams(qfam_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qfam_count, qfams.data());

        for (uint32_t i = 0; i < qfam_count; ++i) {
            if (qfams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 present_support = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_khr_, &present_support);
                if (present_support) {
                    phys_device_ = dev;
                    queue_family_ = i;
                    std::println(stdout, "Sigil: Using device: {}", props.deviceName);
                    break;
                }
            }
        }
        if (phys_device_ != VK_NULL_HANDLE) break;
    }

    if (phys_device_ == VK_NULL_HANDLE) {
        std::println(stderr, "Sigil: No suitable GPU found");
        return std::unexpected(SigilError::DeviceInitFailed);
    }

    // Create logical device
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo qinfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family_,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    std::vector<const char*> dev_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo dev_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qinfo,
        .enabledExtensionCount = static_cast<uint32_t>(dev_extensions.size()),
        .ppEnabledExtensionNames = dev_extensions.data(),
        .pEnabledFeatures = &features,
    };

    if (vkCreateDevice(phys_device_, &dev_info, nullptr, &device_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create logical device");
        return std::unexpected(SigilError::DeviceInitFailed);
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &graphics_queue_);

    return {};
}

std::expected<void, SigilError> Sigil::init_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_device_, surface_khr_, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_device_, surface_khr_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_device_, surface_khr_, &fmt_count, formats.data());

    VkSurfaceFormatKHR chosen_format = formats[0];
    for (auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = fmt;
            break;
        }
    }
    swapchain_format_ = chosen_format.format;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(width_, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height_, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    swapchain_extent_ = extent;
    width_ = extent.width;
    height_ = extent.height;

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR swap_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_khr_,
        .minImageCount = image_count,
        .imageFormat = swapchain_format_,
        .imageColorSpace = chosen_format.colorSpace,
        .imageExtent = swapchain_extent_,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };

    if (vkCreateSwapchainKHR(device_, &swap_info, nullptr, &swapchain_) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create swapchain");
        return std::unexpected(SigilError::SwapchainInitFailed);
    }

    // Get swapchain images
    uint32_t img_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count, nullptr);
    swapchain_images_.resize(img_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &img_count, swapchain_images_.data());

    // Create image views
    swapchain_image_views_.resize(img_count);
    for (uint32_t i = 0; i < img_count; ++i) {
        VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchain_images_[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchain_format_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(device_, &view_info, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
            std::println(stderr, "Sigil: Failed to create image view");
            return std::unexpected(SigilError::SwapchainInitFailed);
        }
    }

    return {};
}

std::expected<void, SigilError> Sigil::init_render_pass() {
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

    VkAttachmentReference color_ref{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

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
        std::println(stderr, "Sigil: Failed to create render pass");
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    return {};
}

std::expected<void, SigilError> Sigil::init_descriptors() {
    // Descriptor set layout
    VkDescriptorSetLayoutBinding sampler_binding{
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };

    VkDescriptorSetLayoutCreateInfo dsl_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &sampler_binding,
    };

    if (vkCreateDescriptorSetLayout(device_, &dsl_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    // Descriptor pool
    VkDescriptorPoolSize pool_size{
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };

    VkDescriptorPoolCreateInfo dp_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };

    if (vkCreateDescriptorPool(device_, &dp_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout_,
    };

    if (vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set_) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    return {};
}

std::expected<void, SigilError> Sigil::init_pipeline() {
    // Read shader SPIR-V files
    auto read_file = [](const char* path) -> std::vector<uint32_t> {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return {};
        size_t size = static_cast<size_t>(file.tellg());
        file.seekg(0);
        std::vector<uint32_t> code(size / sizeof(uint32_t));
        file.read(reinterpret_cast<char*>(code.data()), size);
        return code;
    };

    auto vert_code = read_file("shaders/test.vert.spv");
    auto frag_code = read_file("shaders/test.frag.spv");

    if (vert_code.empty() || frag_code.empty()) {
        std::println(stderr, "Sigil: Failed to load shader SPIR-V files");
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    VkShaderModuleCreateInfo vert_sm{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vert_code.size() * sizeof(uint32_t),
        .pCode = vert_code.data(),
    };
    VkShaderModule vert_module;
    if (vkCreateShaderModule(device_, &vert_sm, nullptr, &vert_module) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    VkShaderModuleCreateInfo frag_sm{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = frag_code.size() * sizeof(uint32_t),
        .pCode = frag_code.data(),
    };
    VkShaderModule frag_module;
    if (vkCreateShaderModule(device_, &frag_sm, nullptr, &frag_module) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_module, nullptr);
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    VkPipelineShaderStageCreateInfo stages[2]{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert_module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag_module,
            .pName = "main",
        },
    };

    // Vertex input
    VkVertexInputBindingDescription binding{
        .binding = 0,
        .stride = sizeof(SigilVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    VkVertexInputAttributeDescription attrs[3]{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(SigilVertex, pos)},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(SigilVertex, uv)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(SigilVertex, color)},
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

    VkViewport viewport{
        .x = 0, .y = 0,
        .width = static_cast<float>(swapchain_extent_.width),
        .height = static_cast<float>(swapchain_extent_.height),
        .minDepth = 0.0f, .maxDepth = 1.0f,
    };

    VkRect2D scissor{
        .offset = {0, 0},
        .extent = swapchain_extent_,
    };

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
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
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
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return std::unexpected(SigilError::PipelineInitFailed);
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
        std::println(stderr, "Sigil: Failed to create graphics pipeline");
        vkDestroyShaderModule(device_, vert_module, nullptr);
        vkDestroyShaderModule(device_, frag_module, nullptr);
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    vkDestroyShaderModule(device_, vert_module, nullptr);
    vkDestroyShaderModule(device_, frag_module, nullptr);

    return {};
}

std::expected<void, SigilError> Sigil::init_framebuffers() {
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
            return std::unexpected(SigilError::PipelineInitFailed);
        }
    }
    return {};
}

std::expected<void, SigilError> Sigil::init_command_pool() {
    VkCommandPoolCreateInfo cp_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_,
    };

    if (vkCreateCommandPool(device_, &cp_info, nullptr, &command_pool_) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    VkCommandBufferAllocateInfo cb_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = MAX_FRAMES_IN_FLIGHT,
    };

    if (vkAllocateCommandBuffers(device_, &cb_info, command_buffers_) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    return {};
}

std::expected<void, SigilError> Sigil::init_sync_objects() {
    VkSemaphoreCreateInfo sem_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(device_, &sem_info, nullptr, &image_available_sems_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &sem_info, nullptr, &render_finished_sems_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fence_info, nullptr, &in_flight_fences_[i]) != VK_SUCCESS) {
            return std::unexpected(SigilError::PipelineInitFailed);
        }
    }

    return {};
}

std::expected<void, SigilError> Sigil::init_atlas_texture() {
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
        return std::unexpected(SigilError::AtlasUploadFailed);
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, atlas_image_, &mem_req);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys_device_, &mem_props);

    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }

    if (mem_type == UINT32_MAX) {
        vkDestroyImage(device_, atlas_image_, nullptr);
        atlas_image_ = VK_NULL_HANDLE;
        return std::unexpected(SigilError::AtlasUploadFailed);
    }

    VkMemoryAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &atlas_memory_) != VK_SUCCESS) {
        vkDestroyImage(device_, atlas_image_, nullptr);
        atlas_image_ = VK_NULL_HANDLE;
        return std::unexpected(SigilError::AtlasUploadFailed);
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

    if (vkCreateImageView(device_, &view_info, nullptr, &atlas_image_view_) != VK_SUCCESS) {
        return std::unexpected(SigilError::AtlasUploadFailed);
    }

    // Sampler
    VkSamplerCreateInfo sampler_info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = 1.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };

    if (vkCreateSampler(device_, &sampler_info, nullptr, &atlas_sampler_) != VK_SUCCESS) {
        return std::unexpected(SigilError::AtlasUploadFailed);
    }

    // Update descriptor set
    VkDescriptorImageInfo desc_img{
        .sampler = atlas_sampler_,
        .imageView = atlas_image_view_,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    VkWriteDescriptorSet write_desc{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set_,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &desc_img,
    };

    vkUpdateDescriptorSets(device_, 1, &write_desc, 0, nullptr);

    return {};
}

std::expected<void, SigilError> Sigil::init_vertex_buffer() {
    // Initial capacity for quads
    vertex_buffer_capacity_ = 4096 * 4 * sizeof(SigilVertex); // 4096 cells * 4 verts

    VkBufferCreateInfo buf_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vertex_buffer_capacity_,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(device_, &buf_info, nullptr, &vertex_buffer_) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, vertex_buffer_, &mem_req);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys_device_, &mem_props);

    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            mem_type = i;
            break;
        }
    }

    if (mem_type == UINT32_MAX) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    VkMemoryAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &vertex_buffer_memory_) != VK_SUCCESS) {
        return std::unexpected(SigilError::PipelineInitFailed);
    }

    vkBindBufferMemory(device_, vertex_buffer_, vertex_buffer_memory_, 0);

    return {};
}

std::expected<void, SigilError> Sigil::upload_atlas(GlyphEngine& glyph_engine) {
    if (!glyph_engine.atlas_dirty()) return {};

    // For simplicity, use a staging buffer approach
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;

    VkBufferCreateInfo buf_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = glyph_engine.atlas_width() * glyph_engine.atlas_height(),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    if (vkCreateBuffer(device_, &buf_info, nullptr, &staging_buffer) != VK_SUCCESS) {
        return std::unexpected(SigilError::AtlasUploadFailed);
    }

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, staging_buffer, &mem_req);

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys_device_, &mem_props);

    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            mem_type = i;
            break;
        }
    }

    if (mem_type == UINT32_MAX) {
        vkDestroyBuffer(device_, staging_buffer, nullptr);
        return std::unexpected(SigilError::AtlasUploadFailed);
    }

    VkMemoryAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &staging_memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging_buffer, nullptr);
        return std::unexpected(SigilError::AtlasUploadFailed);
    }

    vkBindBufferMemory(device_, staging_buffer, staging_memory, 0);

    // Copy atlas data to staging buffer
    void* mapped;
    vkMapMemory(device_, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    std::memcpy(mapped, glyph_engine.atlas_data().data(), glyph_engine.atlas_width() * glyph_engine.atlas_height());
    vkUnmapMemory(device_, staging_memory);

    // Transition image layout to TRANSFER_DST_OPTIMAL
    VkCommandBufferAllocateInfo cb_alloc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBuffer cb;
    vkAllocateCommandBuffers(device_, &cb_alloc, &cb);

    VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    vkBeginCommandBuffer(cb, &begin_info);

    VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = atlas_image_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };

    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {glyph_engine.atlas_width(), glyph_engine.atlas_height(), 1},
    };

    vkCmdCopyBufferToImage(cb, staging_buffer, atlas_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to SHADER_READ_ONLY_OPTIMAL
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };

    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &cb);
    vkDestroyBuffer(device_, staging_buffer, nullptr);
    vkFreeMemory(device_, staging_memory, nullptr);

    glyph_engine.clear_atlas_dirty();

    return {};
}

bool Sigil::process_events() {
    if (closing_) return false;

    wl_display_dispatch_pending(display_);
    wl_display_flush(display_);

    return !closing_;
}

std::expected<void, SigilError> Sigil::render(const Nexus& nexus, GlyphEngine& glyph_engine) {
    // Upload atlas if dirty
    auto at_res = upload_atlas(glyph_engine);
    if (!at_res) return at_res;

    // Ensure vertex buffer is big enough
    uint32_t cell_count = nexus.rows() * nexus.cols();
    size_t needed_size = cell_count * 4 * sizeof(SigilVertex);
    if (needed_size > vertex_buffer_capacity_) {
        if (vertex_buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, vertex_buffer_, nullptr);
            vkFreeMemory(device_, vertex_buffer_memory_, nullptr);
        }
        vertex_buffer_capacity_ = needed_size;

        VkBufferCreateInfo buf_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = vertex_buffer_capacity_,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        vkCreateBuffer(device_, &buf_info, nullptr, &vertex_buffer_);

        VkMemoryRequirements mem_req;
        vkGetBufferMemoryRequirements(device_, vertex_buffer_, &mem_req);

        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(phys_device_, &mem_props);

        uint32_t mem_type = UINT32_MAX;
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((mem_req.memoryTypeBits & (1 << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
                mem_type = i;
                break;
            }
        }

        if (mem_type == UINT32_MAX) {
            std::println(stderr, "Sigil: Failed to find host-visible memory type for vertex buffer");
            return {};
        }

        VkMemoryAllocateInfo alloc_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = mem_type,
        };
        vkAllocateMemory(device_, &alloc_info, nullptr, &vertex_buffer_memory_);
        vkBindBufferMemory(device_, vertex_buffer_, vertex_buffer_memory_, 0);
    }

    // Build vertex data
    cell_size_ = {glyph_engine.cell_width(), glyph_engine.line_height()};
    if (cell_size_.width == 0 || cell_size_.height == 0) return {};

    float cell_w = 2.0f * static_cast<float>(cell_size_.width) / static_cast<float>(width_);
    float cell_h = 2.0f * static_cast<float>(cell_size_.height) / static_cast<float>(height_);

    // Map vertex buffer
    void* mapped;
    vkMapMemory(device_, vertex_buffer_memory_, 0, VK_WHOLE_SIZE, 0, &mapped);
    auto* verts = static_cast<SigilVertex*>(mapped);

    uint32_t vert_count = 0;

    for (uint32_t row = 0; row < nexus.rows(); ++row) {
        for (uint32_t col = 0; col < nexus.cols(); ++col) {
            const auto& cell = nexus.cell_at(row, col);

            if (cell.codepoint == U' ' && cell.fg == Color{255, 255, 255} &&
                cell.bg == Color{0, 0, 0} && !static_cast<bool>(cell.flags & CellFlags::Reverse)) {
                continue; // Skip empty cells
            }

            float x = -1.0f + static_cast<float>(col) * cell_w;
            float y = 1.0f - static_cast<float>(row + 1) * cell_h;

            // Look up glyph
            char32_t cp = cell.codepoint;
            const AtlasGlyph* glyph = glyph_engine.get_glyph(cp);
            if (!glyph) {
                auto result = glyph_engine.cache_glyph(cp);
                if (result) {
                    // Need to get pointer from map since cache_glyph returns by value
                    glyph = glyph_engine.get_glyph(cp);
                    if (!glyph) continue;
                } else {
                    continue;
                }
            }

            // Draw background quad
            if (cell.bg.r > 0 || cell.bg.g > 0 || cell.bg.b > 0 ||
                static_cast<bool>(cell.flags & CellFlags::Reverse)) {
                auto bg = static_cast<bool>(cell.flags & CellFlags::Reverse) ? cell.fg : cell.bg;
                float bg_r = static_cast<float>(bg.r) / 255.0f;
                float bg_g = static_cast<float>(bg.g) / 255.0f;
                float bg_b = static_cast<float>(bg.b) / 255.0f;

                if (vert_count + 4 > cell_count * 4) break;
                setup_quad(&verts[vert_count], col, row, x, y, cell_w, cell_h, nullptr, bg);
                // Override UVs for background (no texture)
                for (int i = 0; i < 4; ++i) {
                    verts[vert_count + i].uv[0] = 0;
                    verts[vert_count + i].uv[1] = 0;
                }
                vert_count += 4;
            }

            // Draw glyph quad
            if (cp != U' ') {
                Color fg = static_cast<bool>(cell.flags & CellFlags::Reverse) ? cell.bg : cell.fg;

                if (vert_count + 4 > cell_count * 4) break;
                setup_quad(&verts[vert_count], col, row, x, y, cell_w, cell_h, glyph, fg);
                vert_count += 4;
            }
        }
    }

    vkUnmapMemory(device_, vertex_buffer_memory_);

    // Frame synchronization
    vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);

    uint32_t image_idx;
    VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                              image_available_sems_[current_frame_],
                                              VK_NULL_HANDLE, &image_idx);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return {};
    } else if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        return {};
    }

    // Record command buffer
    vkResetCommandBuffer(command_buffers_[current_frame_], 0);

    VkCommandBufferBeginInfo begin_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(command_buffers_[current_frame_], &begin_info);

    // Clear + render pass
    VkClearValue clear{{0.05f, 0.05f, 0.1f, 1.0f}}; // Dark terminal background
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

    if (vert_count > 0) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command_buffers_[current_frame_], 0, 1, &vertex_buffer_, &offset);
        vkCmdBindDescriptorSets(command_buffers_[current_frame_], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
        vkCmdDraw(command_buffers_[current_frame_], vert_count, 1, 0, 0);
    }

    vkCmdEndRenderPass(command_buffers_[current_frame_]);
    vkEndCommandBuffer(command_buffers_[current_frame_]);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &image_available_sems_[current_frame_],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffers_[current_frame_],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &render_finished_sems_[current_frame_],
    };

    VkResult submit_res = vkQueueSubmit(graphics_queue_, 1, &submit_info,
                                         in_flight_fences_[current_frame_]);

    VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &render_finished_sems_[current_frame_],
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &image_idx,
    };

    VkResult present = vkQueuePresentKHR(graphics_queue_, &present_info);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }

    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;

    return {};
}

void Sigil::setup_quad(SigilVertex* verts, uint32_t col, uint32_t row,
                        float x, float y, float w, float h,
                        const AtlasGlyph* glyph,
                        const Color& fg) const {
    float r = static_cast<float>(fg.r) / 255.0f;
    float g = static_cast<float>(fg.g) / 255.0f;
    float b = static_cast<float>(fg.b) / 255.0f;

    if (glyph) {
        float u0 = glyph->u0;
        float v0 = glyph->v0;
        float u1 = glyph->u1;
        float v1 = glyph->v1;

        // Top-left
        verts[0] = {{x, y}, {u0, v0}, {r, g, b, 1.0f}};
        // Top-right
        verts[1] = {{x + w, y}, {u1, v0}, {r, g, b, 1.0f}};
        // Bottom-left
        verts[2] = {{x, y + h}, {u0, v1}, {r, g, b, 1.0f}};
        // Bottom-right
        verts[3] = {{x + w, y + h}, {u1, v1}, {r, g, b, 1.0f}};
    } else {
        // Background quad (no UV)
        for (int i = 0; i < 4; ++i) {
            verts[i] = {{0, 0}, {0, 0}, {r, g, b, 1.0f}};
        }
        verts[0].pos[0] = x;       verts[0].pos[1] = y;
        verts[1].pos[0] = x + w;   verts[1].pos[1] = y;
        verts[2].pos[0] = x;       verts[2].pos[1] = y + h;
        verts[3].pos[0] = x + w;   verts[3].pos[1] = y + h;
    }
}

void Sigil::cleanup_swapchain() {
    for (auto& fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();

    for (auto& view : swapchain_image_views_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void Sigil::recreate_swapchain() {
    vkDeviceWaitIdle(device_);

    cleanup_swapchain();

    (void)init_swapchain();
    (void)init_framebuffers();

    // Notify about resize
    if (cell_size_.width > 0 && cell_size_.height > 0) {
        uint32_t cols = width_ / cell_size_.width;
        uint32_t rows = height_ / cell_size_.height;
        if (resize_cb_) resize_cb_(std::max(cols, 1u), std::max(rows, 1u));
    }
}

} // namespace Kaelum
