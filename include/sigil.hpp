#pragma once

#include <expected>
#include <memory>
#include <vector>
#include <functional>
#include <wayland-client.h>
#include <vulkan/vulkan.h>
#include "common.hpp"
#include "nexus.hpp"
#include "glyph_engine.hpp"

struct SigilVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

struct GlyphRect {
    float u0, v0, u1, v1;
};

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

        void handle_key_event(uint32_t key, bool pressed);

        /**
         * @brief Initializes the GPU pipeline and Wayland surface.
         */
        std::expected<void, SigilError> initialize();

        /**
         * @brief Initializes GPU assets like the glyph atlas.
         */
        void initialize_assets(GlyphEngine& engine);

    /**
     * @brief Renders the current Nexus grid to the screen.
     */
    void render(const Nexus& nexus);

    /**
     * @brief Sets a callback for keyboard input events.
     */
    void set_keyboard_callback(std::function<void(uint32_t key, bool pressed)> callback);

    /**
     * @brief Polls Wayland events and updates surface state.
     */
    void poll_events();

    bool prepare_read() { return wl_display_prepare_read(display_) == 0; }
    void cancel_read() { wl_display_cancel_read(display_); }
    void dispatch_pending() { wl_display_dispatch_pending(display_); }
    void flush() { wl_display_flush(display_); }

    /**
     * @brief Returns the Wayland display file descriptor.
     */
    int get_display_fd() const { return wl_display_get_fd(display_); }
    WaylandSurface* get_wl_surface() const { return wl_surface_; }


        /**
         * @brief Handles window resize events.
         */
        void on_resize(uint32_t width, uint32_t height);

        /**
         * @brief Called from xdg_toplevel configure to store dimensions.
         */
        void handle_configure(uint32_t width, uint32_t height);

        /**
         * @brief Process any pending resize (deferred from configure events).
         */
        void process_pending_resize();


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
        struct wl_seat* seat_ = nullptr;
        struct wl_keyboard* keyboard_ = nullptr;

        // Vulkan handles
        VkInstance instance_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphics_queue_ = VK_NULL_HANDLE;
        VkSurfaceKHR vk_surface_ = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkExtent2D extent_ = {0, 0};
        uint32_t configured_width_ = 800;
        uint32_t configured_height_ = 600;
        bool initial_configure_done_ = false;
        bool needs_resize_ = false;
        std::vector<VkImage> swapchain_images_;
        std::vector<VkImageView> swapchain_image_views_;
        std::vector<VkFramebuffer> swapchain_framebuffers_;
        VkRenderPass render_pass_ = VK_NULL_HANDLE;
        VkPipeline graphics_pipeline_ = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
        VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_SRGB;
        uint32_t graphics_queue_family_ = 0;

        // Command buffers
        VkCommandPool command_pool_ = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> command_buffers_;

        // Sync primitives
        VkSemaphore image_available_semaphore_ = VK_NULL_HANDLE;
        VkSemaphore render_finished_semaphore_ = VK_NULL_HANDLE;
        VkFence in_flight_fence_ = VK_NULL_HANDLE;

        // Vertex Buffer
        VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
        VkDeviceMemory vertex_buffer_memory_ = VK_NULL_HANDLE;
        void* vertex_buffer_mapped_ = nullptr;

        // Glyph Atlas
        VkImage glyph_atlas_image_ = VK_NULL_HANDLE;
        VkDeviceMemory glyph_atlas_memory_ = VK_NULL_HANDLE;
        VkImageView glyph_atlas_view_ = VK_NULL_HANDLE;
        VkSampler glyph_atlas_sampler_ = VK_NULL_HANDLE;
        std::map<char32_t, GlyphRect> glyph_map_;

        // Descriptor Sets
        VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

        // Intel Level Zero handles (opaque pointers)
        void* lz_driver_ = nullptr;
        void* lz_device_ = nullptr;

        // Internal Helpers
        std::expected<void, SigilError> init_wayland();
        std::expected<void, SigilError> init_vulkan();
        std::expected<void, SigilError> init_level_zero();
        std::expected<void, SigilError> create_swapchain(VkSwapchainKHR old_swapchain = VK_NULL_HANDLE);
        void create_framebuffers();
        void create_render_pass();
        void create_graphics_pipeline();
        void create_sync_objects();
        void create_command_pool();
        void record_command_buffers();
        void cleanup_swapchain();
        
        uint32_t find_memory_type(uint32_t typeFilter, VkMemoryPropertyFlags properties);
        
        void create_vertex_buffer();

        void create_descriptor_set();
        void create_glyph_atlas(GlyphEngine& engine);

        std::function<void(uint32_t key, bool pressed)> keyboard_callback_ = nullptr;
    };
} // namespace Kaelum

