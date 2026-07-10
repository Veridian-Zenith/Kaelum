#pragma once

#include "common.hpp"
#include "termio.hpp"
#include "renderer.hpp"
#include "glyph_engine.hpp"
#include "expected.hpp"
#include <string>
#include <memory>

namespace Kaelum {

struct SurfaceConfig {
    std::string shell = "sh";
    uint32_t cols = 80;
    uint32_t rows = 24;
    uint32_t cell_width = 13;
    uint32_t cell_height = 17;
    uint32_t width_px = 0;
    uint32_t height_px = 0;
    void* wayland_display = nullptr;
    void* wayland_surface = nullptr;
};

enum class SurfaceError {
    TermioFailed,
    RendererFailed,
    WaylandFailed,
    VulkanFailed,
    GlyphFailed,
};

class Surface {
public:
    Surface() = default;
    ~Surface();

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&&) = delete;
    Surface& operator=(Surface&&) = delete;

    Kaelum::Expected<void, SurfaceError> init(const SurfaceConfig& config);
    void deinit();

    Kaelum::Expected<void, SurfaceError> run_frame(const GlyphEngine& glyph_engine);
    Kaelum::Expected<void, SurfaceError> resize(uint32_t width, uint32_t height);
    Kaelum::Expected<void, SurfaceError> write_input(std::string_view data);
    void commit_surface();

    void* wayland_surface() const { return wayland_surface_; }
    void set_wayland_surface(void* s) { wayland_surface_ = s; }
    void set_wayland_display(void* d) { wayland_display_ = d; }

    const Termio& termio() const { return termio_; }
    Termio& termio() { return termio_; }

private:
    SurfaceConfig config_;
    Termio termio_;
    Renderer renderer_;
    void* wayland_surface_ = nullptr;
    void* wayland_display_ = nullptr;
    void* xdg_surface_ = nullptr;
    void* xdg_toplevel_ = nullptr;
    bool initialized_ = false;
};

} // namespace Kaelum