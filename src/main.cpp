#include "logger.hpp"
#include "surface.hpp"
#include "glyph_engine.hpp"
#include <memory>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

// Forward declaration for frame callback
namespace Kaelum {
    class Application;
}
void frame_callback_done(void* data, struct wl_callback* callback, uint32_t time);

namespace Kaelum {

struct Application {
public:
    Application() = default;
    ~Application() = default;
    
    // Friends for Wayland callbacks
    friend void ::frame_callback_done(void* data, struct wl_callback* callback, uint32_t time);
    
    int run() {
        KAELUM_INFO("Kaelum Terminal v0.1.0");

        // Initialize glyph engine
        glyph_engine_ = std::make_unique<GlyphEngine>();
        auto font_res = glyph_engine_->load_font();
        if (!font_res) {
            KAELUM_WARN("Failed to load font, trying 'monospace'...");
            font_res = glyph_engine_->load_font("monospace");
            if (!font_res) {
                KAELUM_ERROR("Failed to load any font");
                return 1;
            }
        }
        KAELUM_INFO("Font loaded: {}x{} cell size",
                    glyph_engine_->cell_width(), glyph_engine_->line_height());

        // Initialize Wayland
        wayland_display_ = wl_display_connect(nullptr);
        if (!wayland_display_) {
            KAELUM_ERROR("Failed to connect to Wayland display");
            return 1;
        }
        KAELUM_INFO("Connected to Wayland display");

        // Get registry and bind globals
        struct wl_registry* registry = wl_display_get_registry(wayland_display_);
        wl_registry_add_listener(registry, &registry_listener_, this);
        wl_display_roundtrip(wayland_display_);

        if (!compositor_ || !xdg_wm_base_) {
            KAELUM_ERROR("Failed to get required Wayland globals (compositor, xdg_wm_base)");
            return 1;
        }

        // Create Wayland surface
        wl_surface_ = wl_compositor_create_surface(compositor_);
        if (!wl_surface_) {
            KAELUM_ERROR("Failed to create Wayland surface");
            return 1;
        }

        // Create xdg_surface
        xdg_surface_ = xdg_wm_base_get_xdg_surface(xdg_wm_base_, wl_surface_);
        if (!xdg_surface_) {
            KAELUM_ERROR("Failed to create xdg_surface");
            return 1;
        }
        xdg_surface_add_listener(xdg_surface_, &xdg_surface_listener_, this);

        // Create xdg_toplevel
        xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
        if (!xdg_toplevel_) {
            KAELUM_ERROR("Failed to create xdg_toplevel");
            return 1;
        }
        xdg_toplevel_add_listener(xdg_toplevel_, &xdg_toplevel_listener_, this);
        xdg_toplevel_set_title(xdg_toplevel_, "Kaelum Terminal");
        xdg_toplevel_set_app_id(xdg_toplevel_, "kaelum");

        // Set initial window geometry
        xdg_surface_set_window_geometry(xdg_surface_, 0, 0, 800, 600);

        // Initial commit to get configure events
        wl_surface_commit(wl_surface_);
        wl_display_roundtrip(wayland_display_);

        // Wait for xdg_toplevel configure to get the correct size
        while (!configured_) {
            int ret = wl_display_dispatch(wayland_display_);
            if (ret == -1) {
                KAELUM_ERROR("Wayland display dispatch failed during configuration");
                return 1;
            }
        }

// Now we have the correct size from xdg_toplevel configure
        KAELUM_INFO("Configured size: {}x{}", pending_width_, pending_height_);

        // Initialize surface with correct size
        SurfaceConfig config;
        config.cell_width = glyph_engine_->cell_width();
        config.cell_height = glyph_engine_->line_height();
        config.width_px = pending_width_;
        config.height_px = pending_height_;
        config.wayland_display = wayland_display_;
        config.wayland_surface = wl_surface_;
        
        // Calculate cols/rows to match configured pixel dimensions
        config.cols = pending_width_ / glyph_engine_->cell_width();
        config.rows = pending_height_ / glyph_engine_->line_height();
        
        surface_ = std::make_unique<Surface>();
        auto surface_res = surface_->init(config);
        if (!surface_res) {
            KAELUM_ERROR("Failed to initialize surface");
            return 1;
        }

        // Pre-cache ASCII glyphs
        for (char32_t cp = 32; cp < 128; ++cp) {
            (void)glyph_engine_->cache_glyph(cp);
        }

        // Test write
        std::string test = "Welcome to Kaelum v0.1.0!\r\n";
        surface_->write_input(test);

        KAELUM_INFO("Kaelum ready. Grid: {}x{}", config.cols, config.rows);

        // Create frame callback
        frame_callback_ = wl_surface_frame(wl_surface_);
        wl_callback_add_listener(frame_callback_, &frame_callback_listener_, this);
        wl_surface_commit(wl_surface_);
        wl_display_flush(wayland_display_);

        // Main loop - run Wayland event loop with frame callback
        running_ = true;
        return main_loop();
    }

    void shutdown() {
        running_ = false;
    }

private:
    // Wayland globals
    struct wl_compositor* compositor_ = nullptr;
    struct xdg_wm_base* xdg_wm_base_ = nullptr;
    struct wl_surface* wl_surface_ = nullptr;
    struct xdg_surface* xdg_surface_ = nullptr;
    struct xdg_toplevel* xdg_toplevel_ = nullptr;
    struct wl_seat* seat_ = nullptr;

    // Registry listener
    static void registry_global(void* data, struct wl_registry* registry,
                                 uint32_t name, const char* interface, uint32_t version) {
        auto* app = static_cast<Application*>(data);
        if (strcmp(interface, "wl_compositor") == 0) {
            app->compositor_ = static_cast<struct wl_compositor*>(
                wl_registry_bind(registry, name, &wl_compositor_interface, 1));
        } else if (strcmp(interface, "xdg_wm_base") == 0) {
            app->xdg_wm_base_ = static_cast<struct xdg_wm_base*>(
                wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
            xdg_wm_base_add_listener(app->xdg_wm_base_, &xdg_wm_base_listener_, app);
        } else if (strcmp(interface, "wl_seat") == 0) {
            app->seat_ = static_cast<struct wl_seat*>(
                wl_registry_bind(registry, name, &wl_seat_interface, 1));
        }
    }

    static void registry_global_remove(void* data, struct wl_registry* registry,
                                        uint32_t name) {}

    static const struct wl_registry_listener registry_listener_;

    // xdg_wm_base listener
    static void xdg_wm_base_ping(void* data, struct xdg_wm_base* xdg_wm_base,
                                  uint32_t serial) {
        xdg_wm_base_pong(xdg_wm_base, serial);
    }

    static const struct xdg_wm_base_listener xdg_wm_base_listener_;

    // xdg_surface listener
    static void xdg_surface_configure(void* data, struct xdg_surface* xdg_surface,
                                       uint32_t serial) {
        auto* app = static_cast<Application*>(data);
        xdg_surface_ack_configure(xdg_surface, serial);
        
        if (!app->configured_) {
            app->configured_ = true;
            // Need to render first frame after ack_configure
            app->needs_first_render_ = true;
        }
    }

    static const struct xdg_surface_listener xdg_surface_listener_;

    // xdg_toplevel listener
    static void xdg_toplevel_configure(void* data, struct xdg_toplevel* xdg_toplevel,
                                        int32_t width, int32_t height,
                                        struct wl_array* states) {
        auto* app = static_cast<Application*>(data);
        if (width > 0 && height > 0) {
            KAELUM_DEBUG("xdg_toplevel configure: {}x{}", width, height);
            app->pending_width_ = width;
            app->pending_height_ = height;
            app->resize_pending_ = true;
            // Apply immediately
            if (app->surface_) {
                app->surface_->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            }
        }
    }

    static void xdg_toplevel_close(void* data, struct xdg_toplevel* xdg_toplevel) {
        auto* app = static_cast<Application*>(data);
        app->running_ = false;
    }

    static void xdg_toplevel_configure_bounds(void* data, struct xdg_toplevel* xdg_toplevel,
                                               int32_t width, int32_t height) {}

    static void xdg_toplevel_wm_capabilities(void* data, struct xdg_toplevel* xdg_toplevel,
                                              struct wl_array* capabilities) {}

    static const struct xdg_toplevel_listener xdg_toplevel_listener_;

    // Frame callback
    static void frame_callback_done(void* data, struct wl_callback* callback, uint32_t time) {
        auto* app = static_cast<Application*>(data);
        KAELUM_DEBUG("Frame callback fired at time {}", time);
        wl_callback_destroy(callback);
        app->frame_done_ = true;
        
        if (app->running_) {
            struct wl_callback* new_callback = wl_surface_frame(app->wl_surface_);
            wl_callback_add_listener(new_callback, &Application::frame_callback_listener_, app);
        }
    }

    static const struct wl_callback_listener frame_callback_listener_;

    std::unique_ptr<GlyphEngine> glyph_engine_;
    std::unique_ptr<Surface> surface_;
    struct wl_display* wayland_display_ = nullptr;
    struct wl_callback* frame_callback_ = nullptr;
    bool running_ = false;
    bool configured_ = false;
    bool needs_first_render_ = false;
    bool frame_done_ = false;
    bool resize_pending_ = false;
    int32_t pending_width_ = 800;
    int32_t pending_height_ = 600;

    int main_loop() {
        // Get the Wayland display fd
        int wayland_fd = wl_display_get_fd(wayland_display_);
        
        // Get the PTY fd from subprocess
        int pty_fd = -1;
        if (surface_) {
            pty_fd = surface_->termio().pty_fd();
        }
        
        KAELUM_INFO("Starting main loop (wayland_fd={}, pty_fd={})", wayland_fd, pty_fd);
        
        // Render first frame immediately (don't wait for frame callback)
        // The first frame callback will fire after the first buffer is presented
        if (surface_) {
            KAELUM_INFO("Rendering first frame immediately");
            auto io_res = surface_->run_frame(*glyph_engine_);
            if (!io_res) {
                KAELUM_WARN("First frame error");
            }
            wl_surface_commit(wl_surface_);
            // Request frame callback for subsequent frames
            struct wl_callback* callback = wl_surface_frame(wl_surface_);
            wl_callback_add_listener(callback, &frame_callback_listener_, this);
        }
        
        while (running_) {
            // Prepare to read Wayland events
            if (wl_display_prepare_read(wayland_display_) != 0) {
                // Events already queued, dispatch them
                int ret = wl_display_dispatch_pending(wayland_display_);
                if (ret == -1) {
                    KAELUM_ERROR("wl_display_dispatch_pending failed");
                    break;
                }
            } else {
                // Wait for events on either fd
                struct pollfd fds[2] = {
                    {.fd = wayland_fd, .events = POLLIN},
                    {.fd = pty_fd, .events = POLLIN}
                };
                
                int poll_count = 1;
                if (pty_fd >= 0) poll_count = 2;
                
                int ret = poll(fds, poll_count, 16); // ~60fps
                
                if (ret == -1) {
                    if (errno == EINTR) continue;
                    KAELUM_ERROR("poll failed: {}", strerror(errno));
                    break;
                }
                
                if (ret == 0) {
                    // Timeout - no events, continue
                }
                
                // Check Wayland fd
                if (fds[0].revents & POLLIN) {
                    if (wl_display_read_events(wayland_display_) == -1) {
                        KAELUM_ERROR("wl_display_read_events failed");
                        break;
                    }
                    int dispatch_ret = wl_display_dispatch_pending(wayland_display_);
                    if (dispatch_ret == -1) {
                        KAELUM_ERROR("wl_display_dispatch_pending failed");
                        break;
                    }
                }
                
                // Check PTY fd
if (poll_count == 2 && (fds[1].revents & POLLIN)) {
                    if (surface_) {
                        surface_->termio().process_io();
                    }
                }
            }
            
            // Handle pending resize
            if (resize_pending_ && surface_) {
                surface_->resize(static_cast<uint32_t>(pending_width_), static_cast<uint32_t>(pending_height_));
                resize_pending_ = false;
            }

            // Render frame when frame callback fires
            if (surface_ && frame_done_) {
                KAELUM_DEBUG("Rendering frame (frame_done_=true)");
                frame_done_ = false;
                auto io_res = surface_->run_frame(*glyph_engine_);
                if (!io_res) {
                    KAELUM_WARN("Frame error");
                }
                // Request next frame
                struct wl_callback* callback = wl_surface_frame(wl_surface_);
                wl_callback_add_listener(callback, &frame_callback_listener_, this);
            }
        }

        return 0;
    }
};

const struct wl_registry_listener Application::registry_listener_ = {
    .global = Application::registry_global,
    .global_remove = Application::registry_global_remove,
};

const struct xdg_wm_base_listener Application::xdg_wm_base_listener_ = {
    .ping = Application::xdg_wm_base_ping,
};

const struct xdg_surface_listener Application::xdg_surface_listener_ = {
    .configure = Application::xdg_surface_configure,
};

const struct xdg_toplevel_listener Application::xdg_toplevel_listener_ = {
    .configure = Application::xdg_toplevel_configure,
    .close = Application::xdg_toplevel_close,
    .configure_bounds = Application::xdg_toplevel_configure_bounds,
    .wm_capabilities = Application::xdg_toplevel_wm_capabilities,
};

const struct wl_callback_listener Application::frame_callback_listener_ = {
    .done = Application::frame_callback_done,
};

static Application* g_app = nullptr;

void signal_handler(int) {
    if (g_app) g_app->shutdown();
}

} // namespace Kaelum

int main() {
    Kaelum::g_app = new Kaelum::Application();
    std::signal(SIGINT, Kaelum::signal_handler);
    std::signal(SIGTERM, Kaelum::signal_handler);

    int ret = Kaelum::g_app->run();
    delete Kaelum::g_app;
    Kaelum::g_app = nullptr;
    return ret;
}