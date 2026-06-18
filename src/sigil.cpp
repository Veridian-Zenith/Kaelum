#include "sigil.hpp"
#include "wayland-protocols/xdg-shell-client-protocol.hpp"
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <iostream>
#include <print>
#include <vector>
#include <algorithm>
#include <map>

#ifdef KAELUM_WITH_LEVEL_ZERO
#include <ze_api.h>
#include <ze_intel_gpu.h>
#endif

namespace Kaelum {

Sigil::Sigil() = default;

Sigil::~Sigil() {
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (graphics_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, graphics_pipeline_, nullptr);
    if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
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
    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);

    xdg_toplevel_set_title(xdg_toplevel_, "Kaelum");
    xdg_toplevel_set_app_id(xdg_toplevel_, "org.veridian.kaelum");

    wl_surface_commit(wl_surface_);

    std::println("Sigil: Wayland surface created and mapped.");
    return {};
}

std::expected<void, SigilError> Sigil::init_vulkan() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Kaelum";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) return std::unexpected(SigilError::VulkanInitFailed);

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    physical_device_ = devices[0];

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

void Sigil::render(const Nexus& nexus) {
    // Rendering implementation using Vulkan and Level Zero
}

void Sigil::on_resize(uint32_t width, uint32_t height) {
    // Handle swapchain recreation
}

} // namespace Kaelum
