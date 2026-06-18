#include "sigil.hpp"
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <iostream>
#include <print>
#include <vector>
#include <algorithm>

#ifdef KAELUM_WITH_LEVEL_ZERO
#include <ze_api.h>
#include <ze_intel_gpu.h>
#endif

namespace Kaelum {

struct VulkanState {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
};

struct LevelZeroState {
    ze_driver_handle_t driver = nullptr;
    ze_device_handle_t device = nullptr;
};

static VulkanState vk_state;
static LevelZeroState lz_state;

Sigil::Sigil() = default;

Sigil::~Sigil() {
    if (vk_state.device != VK_NULL_HANDLE) {
        vkDestroyDevice(vk_state.device, nullptr);
    }
    if (vk_state.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(vk_state.instance, nullptr);
    }
    // Level Zero handles (driver/device) are managed by the loader and don't require explicit destroy.
}

std::expected<void, SigilError> Sigil::initialize() {
    std::println("Sigil: Initializing Vulkan and Intel Level Zero...");

    // 1. Initialize Vulkan Instance
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Kaelum";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "No Engine";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    if (vkCreateInstance(&create_info, nullptr, &vk_state.instance) != VK_SUCCESS) {
        std::println(stderr, "Sigil: Failed to create Vulkan instance.");
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    // 2. Select Physical Device
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(vk_state.instance, &device_count, nullptr);
    if (device_count == 0) {
        std::println(stderr, "Sigil: No GPUs with Vulkan support found.");
        return std::unexpected(SigilError::VulkanInitFailed);
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(vk_state.instance, &device_count, devices.data());
    
    // For now, just pick the first one (in production, we'd look for Intel XE)
    vk_state.physical_device = devices[0];

    // 3. Initialize Intel Level Zero (if enabled)
#ifdef KAELUM_WITH_LEVEL_ZERO
    if (zeInit(0) != ZE_RESULT_SUCCESS) {
        std::println(stderr, "Sigil: Failed to initialize Level Zero.");
        return std::unexpected(SigilError::LevelZeroInitFailed);
    }

    uint32_t driver_count = 0;
    zeDriverGet(&driver_count, nullptr);
    if (driver_count > 0) {
        std::vector<ze_driver_handle_t> drivers(driver_count);
        zeDriverGet(&driver_count, drivers.data());
        lz_state.driver = drivers[0];

        uint32_t device_count_lz = 0;
        zeDeviceGet(lz_state.driver, &device_count_lz, nullptr);
        
        if (device_count_lz > 0) {
            std::vector<ze_device_handle_t> devices_lz(device_count_lz);
            zeDeviceGet(lz_state.driver, &device_count_lz, devices_lz.data());
            lz_state.device = devices_lz[0];
            std::println("Sigil: Intel Level Zero device initialized.");
        }
    }
#endif

    std::println("Sigil: Hardware initialization complete.");
    return {};
}

void Sigil::render(const Nexus& nexus) {
    // Rendering logic will be implemented here.
    // It will use the Nexus grid and Vulkan/Level Zero to draw glyphs.
}

} // namespace Kaelum
