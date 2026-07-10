#pragma once

#include "common.hpp"
#include "expected.hpp"
#include <string>
#include <vector>
#include <memory>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

namespace Kaelum {

class GlyphEngine;

class Renderer {
public:
    struct Config {
        uint32_t width = 800;
        uint32_t height = 600;
        uint32_t cell_width = 13;
        uint32_t cell_height = 17;
    };

    struct DrawCommand {
        enum class Type { Glyph, Rect };
        Type type;
        uint32_t x = 0, y = 0;
        uint32_t w = 0, h = 0;
        char32_t codepoint = 0;
        Color fg{255, 255, 255};
        Color bg{0, 0, 0};
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    };

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    Kaelum::Expected<void, std::string> init(const Config& config);
    void deinit();

    Kaelum::Expected<void, std::string> render(const std::vector<DrawCommand>& cmds, const GlyphEngine& glyphs);
    Kaelum::Expected<void, std::string> resize(uint32_t width, uint32_t height, uint32_t cell_w, uint32_t cell_h);
    void upload_atlas(const GlyphEngine& glyphs);

    VkSurfaceKHR wayland_surface() const { return surface_; }
    void set_wayland_surface(VkSurfaceKHR s) { surface_ = s; }
    void set_wayland_display(wl_display* d) { wayland_display_ = d; }
    void set_wayland_surface_ptr(wl_surface* s) { wayland_surface_ = s; }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    wl_display* wayland_display_ = nullptr;
    wl_surface* wayland_surface_ = nullptr;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffers_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    uint32_t current_frame_ = 0;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence> in_flight_fences_;
    
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_buffer_memory_ = VK_NULL_HANDLE;
    size_t vertex_buffer_capacity_ = 0;
    
    VkImage atlas_image_ = VK_NULL_HANDLE;
    VkDeviceMemory atlas_memory_ = VK_NULL_HANDLE;
    VkImageView atlas_view_ = VK_NULL_HANDLE;
    VkSampler atlas_sampler_ = VK_NULL_HANDLE;

    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{0, 0};

    Config config_;
    
    void cleanup_swapchain();
    Kaelum::Expected<void, std::string> create_swapchain();
    Kaelum::Expected<void, std::string> create_render_pass();
    Kaelum::Expected<void, std::string> create_descriptor_set_layout();
    Kaelum::Expected<void, std::string> create_pipeline();
    Kaelum::Expected<void, std::string> create_framebuffers();
    Kaelum::Expected<void, std::string> create_command_pool();
    Kaelum::Expected<void, std::string> create_sync_objects();
    Kaelum::Expected<void, std::string> create_atlas();
    Kaelum::Expected<void, std::string> create_vertex_buffer();
};

} // namespace Kaelum