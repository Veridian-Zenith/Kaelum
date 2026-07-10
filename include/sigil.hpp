#pragma once

#include <expected>
#include <memory>
#include <vector>
#include <functional>
#include <cstdint>
#include <string>
#include <wayland-client.h>
#include <vulkan/vulkan.h>
#include <xkbcommon/xkbcommon.h>
#include "wayland-protocols/xdg-shell-client-protocol.hpp"
#include "common.hpp"
#include "nexus.hpp"
#include "glyph_engine.hpp"

namespace Kaelum {

struct SigilXkbState {
    xkb_context* ctx = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* state = nullptr;
    xkb_mod_mask_t alt_mod = 0;
    xkb_mod_mask_t ctrl_mod = 0;

    ~SigilXkbState() {
        if (state) xkb_state_unref(state);
        if (keymap) xkb_keymap_unref(keymap);
        if (ctx) xkb_context_unref(ctx);
    }
};

struct SigilVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

enum class SigilError {
    WaylandInitFailed,
    VulkanInitFailed,
    DeviceInitFailed,
    SwapchainInitFailed,
    PipelineInitFailed,
    AtlasUploadFailed,
    ResizeFailed,
};

class Sigil {
public:
    using KeyboardCallback = std::function<void(const std::string& text)>;
    using ResizeCallback = std::function<void(uint32_t cols, uint32_t rows)>;

    Sigil();
    ~Sigil();

    Sigil(const Sigil&) = delete;
    Sigil& operator=(const Sigil&) = delete;
    Sigil(Sigil&&) = delete;
    Sigil& operator=(Sigil&&) = delete;

    std::expected<void, SigilError> init();

    void set_keyboard_callback(KeyboardCallback cb) { keyboard_cb_ = std::move(cb); }
    void set_resize_callback(ResizeCallback cb) { resize_cb_ = std::move(cb); }

    bool process_events();
    std::expected<void, SigilError> render(const Nexus& nexus, GlyphEngine& glyph_engine);

    uint32_t window_width() const { return width_; }
    uint32_t window_height() const { return height_; }

    // Wayland objects
    wl_display* display() { return display_; }
    wl_compositor* compositor() { return compositor_; }
    wl_surface* surface() { return surface_; }

private:
    // Wayland
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_wm_base* xdg_wm_base_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* xdg_toplevel_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_output* output_ = nullptr;
    SigilXkbState xkb_state_;

    uint32_t width_ = 1280;
    uint32_t height_ = 720;
    bool configured_ = false;
    bool closing_ = false;

    KeyboardCallback keyboard_cb_;
    ResizeCallback resize_cb_;

    CellSize cell_size_{};

    // Vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = UINT32_MAX;

    VkSurfaceKHR surface_khr_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D swapchain_extent_{};

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    std::vector<VkFramebuffer> framebuffers_;

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffers_[MAX_FRAMES_IN_FLIGHT]{};
    VkSemaphore image_available_sems_[MAX_FRAMES_IN_FLIGHT]{};
    VkSemaphore render_finished_sems_[MAX_FRAMES_IN_FLIGHT]{};
    VkFence in_flight_fences_[MAX_FRAMES_IN_FLIGHT]{};
    uint32_t current_frame_ = 0;

    // Glyph atlas texture
    VkImage atlas_image_ = VK_NULL_HANDLE;
    VkDeviceMemory atlas_memory_ = VK_NULL_HANDLE;
    VkImageView atlas_image_view_ = VK_NULL_HANDLE;
    VkSampler atlas_sampler_ = VK_NULL_HANDLE;

    // Vertex buffer for grid quads
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_buffer_memory_ = VK_NULL_HANDLE;
    size_t vertex_buffer_capacity_ = 0;

    std::expected<void, SigilError> init_wayland();
    std::expected<void, SigilError> init_vulkan();
    std::expected<void, SigilError> init_swapchain();
    std::expected<void, SigilError> init_render_pass();
    std::expected<void, SigilError> init_pipeline();
    std::expected<void, SigilError> init_framebuffers();
    std::expected<void, SigilError> init_command_pool();
    std::expected<void, SigilError> init_sync_objects();
    std::expected<void, SigilError> init_atlas_texture();
    std::expected<void, SigilError> init_vertex_buffer();
    std::expected<void, SigilError> init_descriptors();

    std::expected<void, SigilError> upload_atlas(GlyphEngine& glyph_engine);

    void cleanup_swapchain();
    void recreate_swapchain();

    void setup_quad(SigilVertex* verts, uint32_t col, uint32_t row,
                    float x, float y, float w, float h,
                    const AtlasGlyph* glyph,
                    const Color& fg) const;

    // Wayland listeners
    static void wl_registry_global(void* data, wl_registry* reg, uint32_t name,
                                    const char* iface, uint32_t version);
    static void wl_registry_global_remove(void* data, wl_registry* reg, uint32_t name);
    static void xdg_surface_configure(void* data, xdg_surface* surf, uint32_t serial);
    static void xdg_toplevel_configure(void* data, xdg_toplevel* top,
                                       int32_t w, int32_t h, wl_array* states);
    static void xdg_toplevel_close(void* data, xdg_toplevel* top);
    static void xdg_toplevel_configure_bounds(void* data, xdg_toplevel* top,
                                               int32_t w, int32_t h);
    static void xdg_toplevel_wm_capabilities(void* data, xdg_toplevel* top,
                                              wl_array* caps);
    static void xdg_wm_base_ping(void* data, xdg_wm_base* wm, uint32_t serial);
    static void wl_keyboard_keymap(void* data, wl_keyboard* kb, uint32_t fmt,
                                    int fd, uint32_t size);
    static void wl_keyboard_enter(void* data, wl_keyboard* kb, uint32_t serial,
                                   wl_surface* surf, wl_array* keys);
    static void wl_keyboard_leave(void* data, wl_keyboard* kb, uint32_t serial,
                                   wl_surface* surf);
    static void wl_keyboard_key(void* data, wl_keyboard* kb, uint32_t serial,
                                 uint32_t time, uint32_t key, uint32_t state);
    static void wl_keyboard_modifiers(void* data, wl_keyboard* kb,
                                       uint32_t serial, uint32_t mods_depressed,
                                       uint32_t mods_latched, uint32_t mods_locked,
                                       uint32_t group);
    static void wl_keyboard_repeat_info(void* data, wl_keyboard* kb,
                                         int32_t rate, int32_t delay);
    static void wl_seat_capabilities(void* data, wl_seat* seat, uint32_t caps);
    static void wl_seat_name(void* data, wl_seat* seat, const char* name);

    struct xdg_wm_base_listener xdg_wm_listener_;
    struct xdg_surface_listener xdg_surf_listener_;
    struct xdg_toplevel_listener xdg_top_listener_;
    struct wl_keyboard_listener kb_listener_;
    struct wl_seat_listener seat_listener_;
};

} // namespace Kaelum
