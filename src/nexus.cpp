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

    // Parse parameters from sequence_buffer_
    std::vector<int> params;
    int current_param = 0;
    bool has_param = false;

    for (uint8_t b : sequence_buffer_) {
        if (b == ';') {
            params.push_back(current_param);
            current_param = 0;
            has_param = false;
        } else {
            current_param = current_param * 10 + (b - '0');
            has_param = true;
        }
    }
    if (has_param) params.push_back(current_param);

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

void Nexus::parse_sgr(std::span<const uint8_t> /*params*/) {
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
    grid_[cursor_y_ * cols_ + cursor_x_] = Cell{cp, fg, bg, attrs};
}

void Nexus::clear_screen() {
    std::fill(grid_.begin(), grid_.end(), Cell{});
    cursor_x_ = 0;
    cursor_y_ = 0;
}

void Nexus::handle_osc(uint8_t c) {
    if (c == '\a') {
        // BEL terminates OSC
        sequence_buffer_.clear();
        current_state_ = State::Ground;
    } else if (c == 27) {
        // ESC inside OSC — likely ST (\e\\). Consume next byte if it's '\\'.
        // For simplicity, just end the OSC now.
        sequence_buffer_.clear();
        current_state_ = State::Ground;
    } else if (c == 0x9c) {
        // ST (C1 control)
        sequence_buffer_.clear();
        current_state_ = State::Ground;
    } else {
        sequence_buffer_.push_back(c);
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
