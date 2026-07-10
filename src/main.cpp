#include <print>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <csignal>
#include <cstdlib>

#include "logger.hpp"
#include "loom.hpp"
#include "nexus.hpp"
#include "sigil.hpp"
#include "glyph_engine.hpp"

namespace Kaelum {

class Application {
public:
    Application() = default;
    ~Application() = default;

    int run() {
        KAELUM_INFO("Kaelum Terminal v0.1.0");

        // Initialize GlyphEngine
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

        KAELUM_INFO("Creating Sigil...");

        // Initialize Sigil (Wayland + Vulkan)
        sigil_ = std::make_unique<Sigil>();
        KAELUM_INFO("Sigil created");

        sigil_->set_keyboard_callback([this](const std::string& text) {
            if (loom_) (void)loom_->write_input(text);
        });
        sigil_->set_resize_callback([this](uint32_t cols, uint32_t rows) {
            if (nexus_) nexus_->set_size(cols, rows);
            if (loom_) loom_->resize_pty(cols, rows);
        });

        KAELUM_INFO("Calling sigil->init...");
        auto sigil_res = sigil_->init();
        KAELUM_INFO("sigil->init returned");
        if (!sigil_res) {
            KAELUM_ERROR("Failed to initialize Sigil");
            return 1;
        }
        KAELUM_INFO("Sigil initialized: {}x{} window",
                 sigil_->window_width(), sigil_->window_height());

        // Initialize Nexus
        nexus_ = std::make_unique<Nexus>();
        uint32_t cols = sigil_->window_width() / std::max(glyph_engine_->cell_width(), 1u);
        uint32_t rows = sigil_->window_height() / std::max(glyph_engine_->line_height(), 1u);
        nexus_->set_size(std::max(cols, 80u), std::max(rows, 24u));

        // Initialize Loom (io_uring PTY)
        loom_ = std::make_unique<Loom>();
        loom_->set_data_callback([this](std::span<const char> data) {
            nexus_->process_bytes(data);
            nexus_->mark_all_dirty();
        });
        auto loom_res = loom_->init();
        if (!loom_res) {
            KAELUM_ERROR("Failed to initialize Loom. Try running without oneAPI/Level Zero.");
            return 1;
        }
        KAELUM_INFO("Loom initialized");

        // Pre-cache ASCII glyphs
        for (char32_t cp = 32; cp < 128; ++cp) {
            (void)glyph_engine_->cache_glyph(cp);
        }

        // Write test content directly to verify rendering works
        std::string test = "Welcome to Kaelum v0.1.0!";
        nexus_->process_bytes({test.data(), test.size()});
        nexus_->mark_all_dirty();

        // Sync resize
        sigil_->set_resize_callback([this](uint32_t c, uint32_t r) {
            if (nexus_) nexus_->set_size(c, r);
            if (loom_) loom_->resize_pty(c, r);
        });

        // Initial resize to match window
        cols = sigil_->window_width() / std::max(glyph_engine_->cell_width(), 1u);
        rows = sigil_->window_height() / std::max(glyph_engine_->line_height(), 1u);
        loom_->resize_pty(std::max(cols, 80u), std::max(rows, 24u));

        KAELUM_INFO("Kaelum ready. Grid: {}x{}", cols, rows);

        // Main loop
        running_ = true;
        return main_loop();
    }

    void shutdown() {
        running_ = false;
    }

private:
    std::unique_ptr<Loom> loom_;
    std::unique_ptr<Nexus> nexus_;
    std::unique_ptr<Sigil> sigil_;
    std::unique_ptr<GlyphEngine> glyph_engine_;
    bool running_ = false;

    int main_loop() {
        auto last_frame = std::chrono::steady_clock::now();
        constexpr auto frame_duration = std::chrono::milliseconds(16);

        while (running_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = now - last_frame;

            if (!sigil_->process_events()) {
                break;
            }

            if (loom_->is_initialized()) {
                (void)loom_->process_io();
            }

            if (elapsed >= frame_duration) {
                last_frame = now;
                    auto render_res = sigil_->render(*nexus_, *glyph_engine_);
                if (!render_res) {
                    LOG_ERROR("Render error");
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        return 0;
    }
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
