#include "surface.hpp"
#include "logger.hpp"
#include <wayland-client.h>

namespace Kaelum {

Surface::~Surface() {
    deinit();
}

Kaelum::Expected<void, SurfaceError> Surface::init(const SurfaceConfig& config) {
    KAELUM_INFO("Initializing surface");
    config_ = config;
    
    // Store Wayland display/surface for renderer
    wayland_display_ = config.wayland_display;
    wayland_surface_ = config.wayland_surface;

    // Set Wayland display/surface on renderer BEFORE init
    if (wayland_display_ && wayland_surface_) {
        renderer_.set_wayland_display(static_cast<wl_display*>(wayland_display_));
        renderer_.set_wayland_surface_ptr(static_cast<wl_surface*>(wayland_surface_));
    }

    // Initialize Termio
    TermioConfig termio_config;
    termio_config.cols = config.cols;
    termio_config.rows = config.rows;
    termio_config.width_px = config.cols * config.cell_width;
    termio_config.height_px = config.rows * config.cell_height;
    
    auto termio_res = termio_.init(termio_config);
    if (!termio_res) {
        KAELUM_ERROR("Failed to initialize termio");
        return Kaelum::make_unexpected(SurfaceError::TermioFailed);
    }

    // Initialize Renderer
    Renderer::Config renderer_config;
    if (config.width_px > 0 && config.height_px > 0) {
        renderer_config.width = config.width_px;
        renderer_config.height = config.height_px;
    } else {
        renderer_config.width = config.cols * config.cell_width;
        renderer_config.height = config.rows * config.cell_height;
    }
    renderer_config.cell_width = config.cell_width;
    renderer_config.cell_height = config.cell_height;
    
    auto renderer_res = renderer_.init(renderer_config);
    if (!renderer_res) {
        KAELUM_ERROR("Failed to initialize renderer: {}", renderer_res.error());
        return Kaelum::make_unexpected(SurfaceError::RendererFailed);
    }

    initialized_ = true;
    KAELUM_INFO("Surface initialized");
    return {};
}

void Surface::deinit() {
    if (initialized_) {
        renderer_.deinit();
        termio_.deinit();
        initialized_ = false;
    }
}

Kaelum::Expected<void, SurfaceError> Surface::run_frame(const GlyphEngine& glyph_engine) {
    if (!initialized_) return Kaelum::make_unexpected(SurfaceError::TermioFailed);
    
    // Process PTY I/O
    auto io_res = termio_.process_io();
    if (!io_res) {
        KAELUM_WARN("PTY I/O error");
    }
    
    // Render frame
    // Build draw commands from terminal state
    std::vector<Renderer::DrawCommand> cmds;
    // TODO: Generate draw commands from terminal grid
    
    auto render_res = renderer_.render(cmds, glyph_engine);
    if (!render_res) {
        KAELUM_WARN("Render error: {}", render_res.error());
    }
    
    return {};
}

Kaelum::Expected<void, SurfaceError> Surface::resize(uint32_t width, uint32_t height) {
    KAELUM_INFO("Surface resize to {}x{}", width, height);
    auto res = renderer_.resize(width, height, config_.cell_width, config_.cell_height);
    if (!res) {
        KAELUM_WARN("Renderer resize failed: {}", res.error());
        return Kaelum::make_unexpected(SurfaceError::RendererFailed);
    }
    return {};
}

Kaelum::Expected<void, SurfaceError> Surface::write_input(std::string_view data) {
    auto io_res = termio_.write_input(data);
    if (!io_res) {
        KAELUM_WARN("PTY write error");
        return Kaelum::make_unexpected(SurfaceError::TermioFailed);
    }
    return {};
}

void Surface::commit_surface() {
    if (wayland_surface_) {
        wl_surface_commit(static_cast<wl_surface*>(wayland_surface_));
    }
}

} // namespace Kaelum