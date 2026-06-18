#include <iostream>
#include <vector>
#include <print>
#include <thread>
#include <chrono>
#include "loom.hpp"
#include "nexus.hpp"
#include "sigil.hpp"
#include "glyph_engine.hpp"
 
int main() {
    Kaelum::Loom loom;
    Kaelum::Nexus nexus;
    Kaelum::Sigil sigil;
    Kaelum::GlyphEngine glyphs;
    
    auto glyph_res = glyphs.load_font();
    if (!glyph_res) {
        std::println(stderr, "Failed to load font.");
        return 1;
    }

    auto loom_res = loom.initialize();
    if (!loom_res) {
        std::println(stderr, "Failed to initialize Loom.");
        return 1;
    }

    auto sigil_res = sigil.initialize();
    if (!sigil_res) {
        std::println(stderr, "Warning: Sigil initialization failed (probably swapchain), but continuing to test assets.");
    }

    sigil.initialize_assets(glyphs);
    
    if (!sigil_res) {
        std::println("Asset test complete. Exiting due to Sigil initialization failure.");
        return 1;
    }
    
    if (!sigil_res) {
        std::println("Asset test complete. Exiting due to Sigil initialization failure.");
        return 1;
    }
    
    if (!sigil_res) {
        std::println("Asset test complete. Exiting due to Sigil initialization failure.");
        return 1;
    }
    
    if (!sigil_res) {
        std::println("Asset test complete. Exiting due to Sigil initialization failure.");
        return 1;
    }
    
    if (!sigil_res) {
        std::println("Asset test complete. Exiting due to Sigil initialization failure.");
        return 1;
    }
 
    std::println("Kaelum Loom, Nexus, and Sigil initialized. PTY linked to fish shell.");
    std::println("Press Ctrl+C to exit. Processing output into grid...\n");
 
    std::vector<uint8_t> buffer(4096);
    
    while (true) {
        // 1. Poll Wayland Events
        sigil.poll_events();

        // 2. Poll PTY Input
        auto read_res = loom.poll_read(buffer);
        if (read_res) {
            size_t bytes = *read_res;
            if (bytes > 0) {
                // Feed the data into the Nexus emulator
                auto process_res = nexus.process_input({buffer.data(), bytes});
                if (!process_res) {
                    std::println(stderr, "Nexus parsing error.");
                }
            }
        }
        
        // 3. Render the frame
        sigil.render(nexus);
        
        // High-refresh-rate pacing (approx 1ms sleep to prevent CPU pegging)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
 
    return 0;
}

