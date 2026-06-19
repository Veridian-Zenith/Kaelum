#include "nexus.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>

namespace Kaelum {

Nexus::Nexus() {
    grid_.resize(cols_ * rows_, Cell{});

    // O(1) Dispatch table using member function pointers
    dispatch_table_[static_cast<size_t>(State::Ground)] = &Nexus::handle_ground;
    dispatch_table_[static_cast<size_t>(State::Escape)] = &Nexus::handle_escape;
    dispatch_table_[static_cast<size_t>(State::CSI)]    = &Nexus::handle_csi;
    dispatch_table_[static_cast<size_t>(State::EscapeSkip)] = &Nexus::handle_escape_skip;
    dispatch_table_[static_cast<size_t>(State::OSC)]    = &Nexus::handle_osc;
}

void Nexus::handle_ground(uint8_t c) {
    if (c == 27) {
        current_state_ = State::Escape;
    } else if (c == '\n') {
        cursor_y_++;
        if (cursor_y_ >= rows_) {
            scroll_up();
            cursor_y_ = rows_ - 1;
        }
        cursor_x_ = 0;
    } else if (c == '\r') {
        cursor_x_ = 0;
    } else if (c == '\t') {
        cursor_x_ = std::min(cursor_x_ + (8 - cursor_x_ % 8), cols_ - 1);
    } else if (c == '\b' || c == 0x7f) {
        if (cursor_x_ > 0) cursor_x_--;
    } else if (c < 0x20) {
        // Ignore other control characters
    } else {
        set_cell(static_cast<char32_t>(c), vz_fg, vz_bg, 0);
        cursor_x_++;
        if (cursor_x_ >= cols_) {
            cursor_x_ = 0;
            cursor_y_++;
            if (cursor_y_ >= rows_) {
                scroll_up();
                cursor_y_ = rows_ - 1;
            }
        }
    }
}

void Nexus::handle_escape(uint8_t c) {
    if (c == '[') {
        current_state_ = State::CSI;
        sequence_buffer_.clear();
        csi_prefix_ = 0;
    } else if (c == ']') {
        current_state_ = State::OSC;
        sequence_buffer_.clear();
    } else if (c == '(' || c == ')' || c == '*' || c == '+') {
        // Character set designation — next byte selects the set; skip it
        current_state_ = State::EscapeSkip;
    } else {
        // Single-character escape: M (reverse index), 7/8 (save/restore cursor),
        // =, >, c, etc. — silently ignore for now
        current_state_ = State::Ground;
    }
}

void Nexus::handle_escape_skip(uint8_t /*c*/) {
    current_state_ = State::Ground;
}

void Nexus::handle_csi(uint8_t c) {
    if (c == '?' || c == '>' || c == '!') {
        csi_prefix_ = static_cast<char>(c);
    } else if (std::isdigit(c) || c == ';') {
        sequence_buffer_.push_back(c);
    } else {
        process_csi(c);
        sequence_buffer_.clear();
        csi_prefix_ = 0;
        current_state_ = State::Ground;
    }
}

void Nexus::process_csi(uint8_t final_char) {
    // DEC private mode sequences (CSI ? ...) and secondary DA (CSI > ...) — silently ignore
    if (csi_prefix_ != 0) return;

    // Process CSI sequences
    std::vector<int> params;
    int current_param = 0;
    bool has_param = false;

    for (uint8_t b : sequence_buffer_) {
        if (b == ';') {
            params.push_back(current_param);
            current_param = 0;
            has_param = false;
        } else if (std::isdigit(b)) {
            current_param = current_param * 10 + (b - '0');
            has_param = true;
        }
    }
    if (has_param) params.push_back(current_param);

    if (csi_prefix_ == '?') {
        process_csi_dec_private(final_char, params);
        return;
    }

    switch (final_char) {
        case 'A': // Cursor Up
            move_cursor(0, -(params.empty() ? 1 : params[0]));
            break;
        case 'B': // Cursor Down
            move_cursor(0, (params.empty() ? 1 : params[0]));
            break;
        case 'C': // Cursor Right
            move_cursor((params.empty() ? 1 : params[0]), 0);
            break;
        case 'D': // Cursor Left
            move_cursor(-(params.empty() ? 1 : params[0]), 0);
            break;
        case 'H': // Cursor Position (CUP)
            {
                int row = params.size() > 0 ? params[0] : 1;
                int col = params.size() > 1 ? params[1] : 1;
                cursor_x_ = std::clamp(col - 1, 0, (int)cols_ - 1);
                cursor_y_ = std::clamp(row - 1, 0, (int)rows_ - 1);
            }
            break;
        case 'f': // Cursor Position (HVP) — same as 'H'
            {
                int row = params.size() > 0 ? params[0] : 1;
                int col = params.size() > 1 ? params[1] : 1;
                cursor_x_ = std::clamp(col - 1, 0, (int)cols_ - 1);
                cursor_y_ = std::clamp(row - 1, 0, (int)rows_ - 1);
            }
            break;
        case 'J': // Erase in Display (ED)
            {
                int mode = params.empty() ? 0 : params[0];
                if (mode == 0) {
                    // Clear from cursor to end of screen
                    for (size_t x = cursor_x_; x < cols_; ++x)
                        grid_[cursor_y_ * cols_ + x] = Cell{};
                    for (size_t y = cursor_y_ + 1; y < rows_; ++y)
                        for (size_t x = 0; x < cols_; ++x)
                            grid_[y * cols_ + x] = Cell{};
                } else if (mode == 1) {
                    // Clear from start to cursor
                    for (size_t y = 0; y < cursor_y_; ++y)
                        for (size_t x = 0; x < cols_; ++x)
                            grid_[y * cols_ + x] = Cell{};
                    for (size_t x = 0; x <= cursor_x_; ++x)
                        grid_[cursor_y_ * cols_ + x] = Cell{};
                } else if (mode == 2 || mode == 3) {
                    clear_screen();
                }
            }
            break;
        case 'K': // Erase in Line (EL)
            {
                int mode = params.empty() ? 0 : params[0];
                if (mode == 0) {
                    for (size_t x = cursor_x_; x < cols_; ++x)
                        grid_[cursor_y_ * cols_ + x] = Cell{};
                } else if (mode == 1) {
                    for (size_t x = 0; x <= cursor_x_; ++x)
                        grid_[cursor_y_ * cols_ + x] = Cell{};
                } else if (mode == 2) {
                    for (size_t x = 0; x < cols_; ++x)
                        grid_[cursor_y_ * cols_ + x] = Cell{};
                }
            }
            break;
        case 'L': // Insert Lines
            {
                int n = params.empty() ? 1 : params[0];
                for (int i = 0; i < n && cursor_y_ + 1 < rows_; ++i) {
                    for (size_t y = rows_ - 1; y > cursor_y_; --y)
                        for (size_t x = 0; x < cols_; ++x)
                            grid_[y * cols_ + x] = grid_[(y - 1) * cols_ + x];
                    for (size_t x = 0; x < cols_; ++x)
                        grid_[cursor_y_ * cols_ + x] = Cell{};
                }
            }
            break;
        case 'M': // Delete Lines
            {
                int n = params.empty() ? 1 : params[0];
                for (int i = 0; i < n && cursor_y_ < rows_; ++i) {
                    for (size_t y = cursor_y_; y + 1 < rows_; ++y)
                        for (size_t x = 0; x < cols_; ++x)
                            grid_[y * cols_ + x] = grid_[(y + 1) * cols_ + x];
                    for (size_t x = 0; x < cols_; ++x)
                        grid_[(rows_ - 1) * cols_ + x] = Cell{};
                }
            }
            break;
        case 'P': // Delete Characters
            {
                int n = params.empty() ? 1 : params[0];
                for (size_t x = cursor_x_; x + n < cols_; ++x)
                    grid_[cursor_y_ * cols_ + x] = grid_[cursor_y_ * cols_ + x + n];
                for (size_t x = (cols_ > static_cast<size_t>(n) ? cols_ - n : 0); x < cols_; ++x)
                    grid_[cursor_y_ * cols_ + x] = Cell{};
            }
            break;
        case '@': // Insert Characters
            {
                int n = params.empty() ? 1 : params[0];
                for (size_t x = cols_ - 1; x >= cursor_x_ + n; --x)
                    grid_[cursor_y_ * cols_ + x] = grid_[cursor_y_ * cols_ + x - n];
                for (size_t x = cursor_x_; x < cursor_x_ + static_cast<size_t>(n) && x < cols_; ++x)
                    grid_[cursor_y_ * cols_ + x] = Cell{};
            }
            break;
        case 'd': // Vertical Line Position Absolute (VPA)
            cursor_y_ = std::clamp((params.empty() ? 1 : params[0]) - 1, 0, (int)rows_ - 1);
            break;
        case 'G': // Cursor Character Absolute (CHA)
            cursor_x_ = std::clamp((params.empty() ? 1 : params[0]) - 1, 0, (int)cols_ - 1);
            break;
        case 'm': // Select Graphic Rendition (SGR)
            parse_sgr(sequence_buffer_);
            break;
        case 'r': // Set Scrolling Region (DECSTBM) — ignored for now
            break;
        case 'c': // Device Attributes (DA) — ignored, query from shell
            break;
        case 'n': // Device Status Report — ignored
            break;
        case 'l': // Reset Mode — standard modes, ignored
            break;
        case 'h': // Set Mode — standard modes, ignored
            break;
        case 's': // Save Cursor Position (SCP)
            break;
        case 'u': // Restore Cursor Position (RCP)
            break;
        case 'X': // Erase Characters (ECH)
            {
                int n = params.empty() ? 1 : params[0];
                for (int i = 0; i < n && cursor_x_ + i < cols_; ++i)
                    grid_[cursor_y_ * cols_ + cursor_x_ + i] = Cell{};
            }
            break;
        case 'S': // Scroll Up
            {
                int n = params.empty() ? 1 : params[0];
                for (int i = 0; i < n; ++i) scroll_up();
            }
            break;
        case 'T': // Scroll Down
            break;
        default:
            break;
    }
}

void Nexus::process_csi_dec_private(uint8_t final_char, const std::vector<int>& params) {
    if (params.empty()) return;

    int mode = params[0];
    switch (final_char) {
        case 'h': // Set Mode
            switch (mode) {
                case 25:   /* Enable cursor visibility - ignore for now */ break;
                case 1004: /* Enable focus reporting - ignore for now */ break;
                case 1049: /* Enable alternate screen buffer - ignore for now */ break;
                case 2004: /* Enable bracketed paste - ignore for now */ break;
                case 2031: /* Enable color theme reporting - ignore for now */ break;
            }
            break;
        case 'l': // Reset Mode
            switch (mode) {
                case 25:   /* Disable cursor visibility - ignore for now */ break;
                case 1004: /* Disable focus reporting - ignore for now */ break;
                case 1049: /* Disable alternate screen buffer - ignore for now */ break;
                case 2004: /* Disable bracketed paste - ignore for now */ break;
                case 2031: /* Disable color theme reporting - ignore for now */ break;
            }
            break;
    }
}

void Nexus::parse_sgr(std::span<const uint8_t> params) {
    if (params.empty()) {
        // \e[m : Reset all
        current_fg_ = vz_fg;
        current_bg_ = vz_bg;
        current_attrs_ = 0;
        return;
    }

    // SGR can have multiple parameters separated by ';'
    // Note: the sequence_buffer_ contains the parameters
    // But the current design of process_csi passes the sequence_buffer_ to parse_sgr.
    // Wait, the current process_csi implementation of parse_sgr is:
    // case 'm': parse_sgr(sequence_buffer_); break;
    // And sequence_buffer_ contains the digits and semicolons.
    
    // I need to parse the sequence_buffer_ similarly to how I parsed params in process_csi.
    
    std::vector<int> sgr_params;
    int current_param = 0;
    bool has_param = false;
    for (uint8_t b : params) {
        if (b == ';') {
            sgr_params.push_back(current_param);
            current_param = 0;
            has_param = false;
        } else if (std::isdigit(b)) {
            current_param = current_param * 10 + (b - '0');
            has_param = true;
        }
    }
    if (has_param) sgr_params.push_back(current_param);

    for (int p : sgr_params) {
        if (p == 0) {
            current_fg_ = vz_fg;
            current_bg_ = vz_bg;
            current_attrs_ = 0;
        } else if (p == 1) {
            current_attrs_ |= attr_bold;
        } else if (p == 2) {
            current_attrs_ |= attr_dim;
        } else if (p == 3) {
            current_attrs_ |= attr_italic;
        } else if (p == 4) {
            current_attrs_ |= attr_underline;
        } else if (p == 7) {
            current_attrs_ |= attr_reverse;
        } else if (p == 9) {
            current_attrs_ |= attr_strike;
        } else if (p == 23) {
            current_attrs_ &= ~attr_italic;
        } else if (p == 24) {
            current_attrs_ &= ~attr_underline;
        } else if (p == 29) {
            current_attrs_ &= ~attr_strike;
        } else if (p >= 30 && p <= 37) {
            // Foreground 0-7 (simplified to vz_fg for now, or could add palette)
            current_fg_ = vz_fg; 
        } else if (p >= 40 && p <= 47) {
            // Background 0-7
            current_bg_ = vz_bg;
        } else if (p >= 90 && p <= 97) {
            // Foreground 8-15
            current_fg_ = vz_fg;
        } else if (p >= 100 && p <= 107) {
            // Background 8-15
            current_bg_ = vz_bg;
        } else if (p == 39) {
            current_fg_ = vz_fg;
        } else if (p == 49) {
            current_bg_ = vz_bg;
        }
    }
}

void Nexus::resize(size_t cols, size_t rows) {
    if (cols == 0 || rows == 0) return;
    Grid new_grid(cols * rows, Cell{});
    size_t copy_cols = std::min(cols, cols_);
    size_t copy_rows = std::min(rows, rows_);
    for (size_t y = 0; y < copy_rows; ++y) {
        for (size_t x = 0; x < copy_cols; ++x) {
            new_grid[y * cols + x] = grid_[y * cols_ + x];
        }
    }
    grid_ = std::move(new_grid);
    cols_ = cols;
    rows_ = rows;
    cursor_x_ = std::min(cursor_x_, cols_ - 1);
    cursor_y_ = std::min(cursor_y_, rows_ - 1);
}

void Nexus::move_cursor(int dx, int dy) {
    int new_x = static_cast<int>(cursor_x_) + dx;
    int new_y = static_cast<int>(cursor_y_) + dy;
    cursor_x_ = static_cast<size_t>(std::clamp(new_x, 0, static_cast<int>(cols_) - 1));
    cursor_y_ = static_cast<size_t>(std::clamp(new_y, 0, static_cast<int>(rows_) - 1));
}

void Nexus::scroll_up() {
    std::copy(grid_.begin() + cols_, grid_.end(), grid_.begin());
    std::fill(grid_.end() - cols_, grid_.end(), Cell{});
}

void Nexus::set_cell(char32_t cp, Color fg, Color bg, uint32_t attrs) {
    // Use current state if defaults (vz_fg, vz_bg, 0) are passed
    Color final_fg = (fg == vz_fg) ? current_fg_ : fg;
    Color final_bg = (bg == vz_bg) ? current_bg_ : bg;
    uint32_t final_attrs = (attrs == 0) ? current_attrs_ : attrs;
    
    grid_[cursor_y_ * cols_ + cursor_x_] = Cell{cp, final_fg, final_bg, final_attrs};
}

void Nexus::clear_screen() {
    std::fill(grid_.begin(), grid_.end(), Cell{});
    cursor_x_ = 0;
    cursor_y_ = 0;
}

void Nexus::handle_osc(uint8_t c) {
    if (c == '\a') {
        process_osc();
        sequence_buffer_.clear();
        current_state_ = State::Ground;
    } else if (c == 27) {
        // ESC inside OSC — likely ST (\e\\).
        process_osc();
        sequence_buffer_.clear();
        current_state_ = State::Ground;
    } else if (c == 0x9c) {
        process_osc();
        sequence_buffer_.clear();
        current_state_ = State::Ground;
    } else {
        sequence_buffer_.push_back(c);
    }
}

void Nexus::process_osc() {
    if (sequence_buffer_.empty()) return;

    // OSC sequences start with a parameter (e.g., '0' for window title)
    uint8_t param = sequence_buffer_[0];
    
    switch (param) {
        case 0: // Set window title
            break;
        case 1: // Set tab title
            break;
        case 7: // Report working directory
            break;
        case 8: // Hyperlink
            break;
        case 11: // Query background color
            break;
        case 52: // Copy to clipboard
            break;
        case 133: // Kitty prompt marking (requires more complex parsing)
            break;
    }
}

std::expected<void, NexusError> Nexus::process_input(std::span<const uint8_t> data) {
    for (uint8_t byte : data) {
        auto handler = dispatch_table_[static_cast<size_t>(current_state_)];
        if (handler) {
            (this->*handler)(byte);
        } else {
            current_state_ = State::Ground;
        }
    }
    return {};
}

} // namespace Kaelum
