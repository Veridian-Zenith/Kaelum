#pragma once

#include <expected>
#include <memory>
#include <vector>
#include <wayland-client.h>
#include <vulkan/vulkan.h>
#include "common.hpp"
#include "nexus.hpp"

// Wayland type aliases to avoid namespace collisions
using WaylandDisplay = struct wl_display;
using WaylandSurface = struct wl_surface;
using WaylandCompositor = struct wl_compositor;
using WaylandRegistry = struct wl_registry;
using XdgWmBase = struct xdg_wm_base;
using XdgSurface = struct xdg_surface;
using XdgToplevel = struct xdg_toplevel;

namespace Kaelum {

    enum class SigilError {
        VulkanInitFailed,
        WaylandInitFailed,
        LevelZeroInitFailed,
        AllocationFailed,
        SurfaceCreationFailed
    };

    /**
     * @brief Sigil is the GPU renderer utilizing Vulkan and Intel Level Zero.
     */
    class Sigil {
    public:
        Sigil();
        ~Sigil();

        /**
         * @brief Initializes the GPU pipeline and Wayland surface.
         */
        std::expected<void, SigilError> initialize();

        /**
         * @brief Renders the current Nexus grid to the screen.
         */
        void render(const Nexus& nexus);

        /**
         * @brief Polls Wayland events and updates surface state.
         */
        void poll_events();

        /**
         * @brief Handles window resize events.
         */
        void on_resize(uint32_t width, uint32_t height);


    private:
        friend void registry_handle_global(void* data, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version);
        // Wayland handles
        WaylandDisplay* display_ = nullptr;
        WaylandSurface* wl_surface_ = nullptr;
        WaylandCompositor* compositor_ = nullptr;
        WaylandRegistry* registry_ = nullptr;
        XdgWmBase* xdg_wm_base_ = nullptr;
        XdgSurface* xdg_surface_ = nullptr;
        XdgToplevel* xdg_toplevel_ = nullptr;

        // Vulkan handles
        VkInstance instance_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphics_queue_ = VK_NULL_HANDLE;
        VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkExtent2D extent_ = {0, 0};
        std::vector<VkImage> swapchain_images_;
        std::vector<VkImageView> swapchain_image_views_;
        std::vector<VkFramebuffer> swapchain_framebuffers_;
        VkRenderPass render_pass_ = VK_NULL_HANDLE;
        VkPipeline graphics_pipeline_ = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

        // Command buffers
        VkCommandPool command_pool_ = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> command_buffers_;

        // Sync primitives
        VkSemaphore image_available_semaphore_ = VK_NULL_HANDLE;
        VkSemaphore render_finished_semaphore_ = VK_NULL_HANDLE;
        VkFence in_flight_fence_ = VK_NULL_HANDLE;
        uint32_t current_frame_ = 0;

        // Intel Level Zero handles (opaque pointers)
        void* lz_driver_ = nullptr;
        void* lz_device_ = nullptr;

        // Internal Helpers
        std::expected<void, SigilError> init_wayland();
        std::expected<void, SigilError> init_vulkan();
        std::expected<void, SigilError> init_level_zero();
        void create_swapchain();
        void create_framebuffers();
        void create_render_pass();
        void create_graphics_pipeline();
        void create_sync_objects();
        void create_command_pool();
        void record_command_buffers();
        void cleanup_swapchain();
    };
} // namespace Kaelum

