#include <vector>
#include <print>
#include <map>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
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

    // Set cell dimensions for resize calculation
    uint32_t cw = glyphs.get_cell_width();
    uint32_t ch = glyphs.get_line_height();
    if (cw == 0) cw = 8;
    if (ch == 0) ch = 14;
    sigil.set_cell_size(cw, ch);

    sigil.set_resize_callback([&nexus, &loom](uint32_t cols, uint32_t rows, uint32_t wpx, uint32_t hpx) {
        nexus.resize(cols, rows);
        loom.set_pty_size(static_cast<uint16_t>(cols), static_cast<uint16_t>(rows),
                          static_cast<uint16_t>(wpx), static_cast<uint16_t>(hpx));
    });

    // Sync grid + PTY to the actual window size from the compositor
    {
        uint32_t wpx = sigil.configured_width();
        uint32_t hpx = sigil.configured_height();
        uint32_t cols = (cw > 0) ? wpx / cw : Kaelum::k_default_cols;
        uint32_t rows = (ch > 0) ? hpx / ch : Kaelum::k_default_rows;
        if (cols == 0) cols = 1;
        if (rows == 0) rows = 1;
        nexus.resize(cols, rows);
        loom.set_pty_size(static_cast<uint16_t>(cols), static_cast<uint16_t>(rows),
                          static_cast<uint16_t>(wpx), static_cast<uint16_t>(hpx));
    }

    // Map Linux input scancodes to ASCII
    std::map<uint32_t, char> key_map = {
        // Letters
        {30, 'a'}, {48, 'b'}, {46, 'c'}, {32, 'd'}, {18, 'e'}, {33, 'f'}, {34, 'g'}, {35, 'h'},
        {23, 'i'}, {36, 'j'}, {37, 'k'}, {38, 'l'}, {50, 'm'}, {49, 'n'}, {24, 'o'}, {25, 'p'},
        {16, 'q'}, {19, 'r'}, {31, 's'}, {20, 't'}, {22, 'u'}, {47, 'v'}, {17, 'w'}, {45, 'x'},
        {21, 'y'}, {44, 'z'},
        // Numbers
        {2, '1'}, {3, '2'}, {4, '3'}, {5, '4'}, {6, '5'}, {7, '6'}, {8, '7'}, {9, '8'}, {10, '9'}, {11, '0'},
        // Symbols
        {12, '-'}, {13, '='}, {26, '['}, {27, ']'}, {39, ';'}, {40, '\''}, {41, '`'},
        {43, '\\'}, {51, ','}, {52, '.'}, {53, '/'},
        // Control
        {28, '\n'}, {57, ' '}, {14, '\b'}, {15, '\t'}, {1, '\x1b'},
    };

    // Arrow keys and special keys → escape sequences
    std::map<uint32_t, std::string> special_key_map = {
        {103, "\x1b[A"}, // Up
        {108, "\x1b[B"}, // Down
        {106, "\x1b[C"}, // Right
        {105, "\x1b[D"}, // Left
        {102, "\x1b[H"}, // Home
        {107, "\x1b[F"}, // End
        {104, "\x1b[5~"}, // Page Up
        {109, "\x1b[6~"}, // Page Down
        {111, "\x1b[3~"}, // Delete
    };

    sigil.set_keyboard_callback([&loom, &key_map, &special_key_map](uint32_t key, bool pressed) {
        if (!pressed) return;
        if (special_key_map.contains(key)) {
            const auto& seq = special_key_map[key];
            (void)loom.write({(const uint8_t*)seq.data(), seq.size()});
        } else if (key_map.contains(key)) {
            char c = key_map[key];
            (void)loom.write({(uint8_t*)&c, 1});
        }
    });

    std::println("Kaelum Loom, Nexus, and Sigil initialized. PTY linked to fish shell.");

    std::println("Press Ctrl+C to exit. Processing output into grid...\n");
 
    std::vector<uint8_t> buffer(4096);
    
    int wayland_fd = sigil.get_display_fd();
    int pty_fd = loom.master_fd();

    // Poll on Wayland display FD + PTY master FD directly
    std::vector<pollfd> poll_fds = {
        { wayland_fd, POLLIN, 0 },
        { pty_fd, POLLIN, 0 }
    };

    while (!sigil.should_close()) {
        sigil.dispatch_pending();
        sigil.flush();

        if (!sigil.prepare_read()) {
            sigil.dispatch_pending();
            sigil.process_pending_resize();
            sigil.render(nexus);
            continue;
        }

        int ret = poll(poll_fds.data(), poll_fds.size(), 16);

        if (ret < 0) {
            if (errno == EINTR) {
                sigil.cancel_read();
                continue;
            }
            sigil.cancel_read();
            std::println(stderr, "Poll error.");
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            sigil.poll_events();
        } else {
            sigil.cancel_read();
        }

        // Read PTY output directly — standard read() + poll is what
        // Ghostty, Kitty, and every production terminal uses
        if (poll_fds[1].revents & POLLIN) {
            ssize_t n = ::read(pty_fd, buffer.data(), buffer.size());
            if (n > 0) {
                auto process_res = nexus.process_input({buffer.data(), static_cast<size_t>(n)});
                if (!process_res) {
                    std::println(stderr, "Nexus parsing error.");
                }
            } else if (n == 0) {
                // Shell exited
                break;
            }
            // n < 0 && errno == EAGAIN: no data, continue
        }

        sigil.process_pending_resize();
        sigil.render(nexus);
    }
 
    return 0;
}

