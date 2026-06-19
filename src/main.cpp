#include <vector>
#include <print>
#include <map>
#include <poll.h>
#include <unistd.h>
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
        std::println(stderr, "Fatal: Sigil initialization failed.");
        return 1;
    }

    sigil.initialize_assets(glyphs);

    
    // Map wayland keycodes to ASCII
    std::map<uint32_t, char> key_map = {
        {30, 'a'}, {48, 'b'}, {46, 'c'}, {32, 'd'}, {18, 'e'}, {33, 'f'}, {34, 'g'}, {35, 'h'}, 
        {23, 'i'}, {36, 'j'}, {37, 'k'}, {38, 'l'}, {50, 'm'}, {49, 'n'}, {24, 'o'}, {25, 'p'}, 
        {16, 'q'}, {19, 'r'}, {31, 's'}, {20, 't'}, {22, 'u'}, {47, 'v'}, {17, 'w'}, {45, 'x'}, 
        {21, 'y'}, {44, 'z'}, {28, '\n'}, {57, ' '}
    };

    sigil.set_keyboard_callback([&loom, &key_map](uint32_t key, bool pressed) {
        if (pressed && key_map.contains(key)) {
            char c = key_map[key];
            (void)loom.write({(uint8_t*)&c, 1});
        }
    });

    std::println("Kaelum Loom, Nexus, and Sigil initialized. PTY linked to fish shell.");

    std::println("Press Ctrl+C to exit. Processing output into grid...\n");
 
    std::vector<uint8_t> buffer(4096);
    
    // Event-driven setup
    int wayland_fd = sigil.get_display_fd();
    auto wake_fd_res = loom.register_wake_fd();
    if (!wake_fd_res) {
        std::println(stderr, "Failed to register io_uring wake FD.");
        return 1;
    }
    int wake_fd = *wake_fd_res;

    std::vector<pollfd> poll_fds = {
        { wayland_fd, POLLIN, 0 },
        { wake_fd, POLLIN, 0 }
    };

    while (true) {
        sigil.dispatch_pending();
        sigil.flush();

        if (!sigil.prepare_read()) {
            sigil.dispatch_pending();
            sigil.process_pending_resize();
            sigil.render(nexus);
            continue;
        }

        int ret = poll(poll_fds.data(), poll_fds.size(), 1);

        if (ret < 0) {
            sigil.cancel_read();
            std::println(stderr, "Poll error.");
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            sigil.poll_events();
        } else {
            sigil.cancel_read();
        }

        if (poll_fds[1].revents & POLLIN) {
            // Consume the eventfd to prevent busy-loop
            uint64_t wake_val;
            [[maybe_unused]] auto r = ::read(wake_fd, &wake_val, sizeof(wake_val));

            auto read_res = loom.poll_read(buffer);
            if (read_res) {
                size_t bytes = *read_res;
                if (bytes > 0) {
                    auto process_res = nexus.process_input({buffer.data(), bytes});
                    if (!process_res) {
                        std::println(stderr, "Nexus parsing error.");
                    }
                }
            }
        }

        sigil.process_pending_resize();
        sigil.render(nexus);
    }
 
    return 0;
}

