#include "nexus.hpp"
#include <algorithm>
#include <cctype>
#include <print>
#include <cstring>
#include <cmath>

namespace Kaelum {

Nexus::Nexus() {
    grid_.resize(cols_ * rows_);
}

Nexus::~Nexus() = default;

void Nexus::set_size(uint32_t cols, uint32_t rows) {
    if (cols == cols_ && rows == rows_) return;

    std::vector<ScreenChar> old_grid = std::move(grid_);
    uint32_t old_cols = cols_;
    uint32_t old_rows = rows_;

    cols_ = std::max(cols, 1u);
    rows_ = std::max(rows, 1u);
    grid_.resize(cols_ * rows_);

    // Copy old content
    for (uint32_t r = 0; r < std::min(old_rows, rows_); ++r) {
        for (uint32_t c = 0; c < std::min(old_cols, cols_); ++c) {
            grid_[r * cols_ + c] = old_grid[r * old_cols + c];
        }
    }

    if (cursor_.row >= rows_) cursor_.row = rows_ - 1;
    if (cursor_.col >= cols_) cursor_.col = cols_ - 1;

    mark_all_dirty();
}

void Nexus::mark_all_dirty() {
    for (auto& cell : grid_) cell.dirty = true;
}

uint32_t Nexus::idx(uint32_t row, uint32_t col) const {
    return row * cols_ + col;
}

const ScreenChar& Nexus::cell_at(uint32_t row, uint32_t col) const {
    if (row < rows_ && col < cols_) {
        return grid_[idx(row, col)];
    }
    static ScreenChar default_cell;
    return default_cell;
}

void Nexus::process_bytes(std::span<const char> data) {
    for (char c : data) {
        process_byte(c);
    }
}

void Nexus::process_byte(char c) {
    unsigned char uc = static_cast<unsigned char>(c);

    switch (state_) {
    case State::Ground:
        if (uc < 0x20) {
            handle_c0(c);
        } else if (uc == 0x7F) {
            // DEL - ignore
        } else if (uc >= 0x80 && uc <= 0x9F) {
            // C1 control characters
            handle_c0(static_cast<char>(uc - 0x40));
        } else {
            handle_printable(static_cast<char32_t>(uc));
        }
        break;

    case State::Escape:
        if (uc == '[') {
            state_ = State::CSI;
            csi_ = CsiParam{};
        } else if (uc == ']') {
            state_ = State::Osc;
            osc_.data.clear();
        } else if (uc == 'P') {
            state_ = State::Dcs;
        } else if (uc == 'X') {
            state_ = State::Sos;
        } else if (uc == '^') {
            state_ = State::Pm;
        } else if (uc == '_') {
            state_ = State::Apc;
        } else if (uc >= ' ' && uc <= '/') {
            // Intermediate bytes, consume and go to EscapeSkip
            escape_buf_.clear();
            escape_buf_.push_back(c);
            state_ = State::EscapeSkip;
        } else {
            dispatch_escape(c);
            state_ = State::Ground;
        }
        break;

    case State::EscapeSkip:
        if (uc >= ' ' && uc <= '/') {
            escape_buf_.push_back(c);
        } else if (uc >= '0' && uc <= '~') {
            // Complete escape sequence with intermediate
            dispatch_escape(c);
            state_ = State::Ground;
        } else {
            state_ = State::Ground;
        }
        break;

    case State::CSI:
        if (uc >= '0' && uc <= '9') {
            if (csi_.param_count == 0) csi_.param_count = 1;
            int idx = csi_.param_count - 1;
            csi_.params[idx] = csi_.params[idx] * 10 + (uc - '0');
        } else if (uc == ';') {
            if (csi_.param_count < 16) csi_.param_count++;
            if (csi_.param_count > 0) csi_.params[csi_.param_count - 1] = 0;
        } else if (uc >= ' ' && uc <= '/') {
            csi_.intermediate = c;
        } else if (uc >= '@' && uc <= '~') {
            csi_.final_byte = c;
            dispatch_csi();
            state_ = State::Ground;
        } else if (uc >= '<' && uc <= '?') {
            // Private marker
            // Store in intermediate for now
            csi_.intermediate = c;
        } else {
            state_ = State::Ground;
        }
        break;

    case State::Osc:
        if (uc == 0x07 || (uc == 0x1B && !pending_input_.empty())) {
            // ST (string terminator)
            if (uc == 0x1B) {
                // Need to consume the backslash too
                pending_input_.push_back(c);
                break;
            }
            osc_dispatch();
            state_ = State::Ground;
        } else if (uc == '\\' && !pending_input_.empty() && pending_input_.back() == '\x1B') {
            // ST via ESC \ - remove the ESC
            osc_.data.pop_back(); // Remove ESC
            osc_dispatch();
            state_ = State::Ground;
        } else {
            osc_.data.push_back(c);
        }
        break;

    case State::Dcs:
    case State::Sos:
    case State::Pm:
    case State::Apc:
        // For now, consume until ST
        if (uc == 0x1B) {
            pending_input_.push_back(c);
        } else if (uc == '\\' && !pending_input_.empty() && pending_input_.back() == '\x1B') {
            state_ = State::Ground;
            pending_input_.clear();
        } else if (uc == 0x07) {
            state_ = State::Ground;
            pending_input_.clear();
        } else {
            if (!pending_input_.empty()) pending_input_.clear();
        }
        break;
    }
}

void Nexus::handle_c0(char c) {
    switch (c) {
    case '\x00': break; // NUL
    case '\x07': break; // BEL - ignore for now
    case '\x08': backspace(); break;
    case '\x09': tab(); break;
    case '\x0A': new_line(); break;
    case '\x0B': new_line(); break; // VT
    case '\x0C': new_line(); break; // FF
    case '\x0D': carriage_return(); break;
    case '\x0E': break; // SO
    case '\x0F': break; // SI
    case '\x1B': state_ = State::Escape; break;
    default: break;
    }
}

void Nexus::handle_printable(char32_t c) {
    put_char(c);
}

void Nexus::put_char(char32_t c) {
    if (cursor_.row >= rows_ || cursor_.col >= cols_) {
        return;
    }

    auto& cell = grid_[idx(cursor_.row, cursor_.col)];
    cell.codepoint = c;
    cell.fg = fg_;
    cell.bg = bg_;
    cell.flags = flags_;
    cell.dirty = true;

    cursor_.col++;
    if (cursor_.col >= cols_) {
        new_line();
    }
}

void Nexus::new_line() {
    cursor_.col = 0;
    if (cursor_.row + 1 >= rows_) {
        scroll_up();
    } else {
        cursor_.row++;
    }
}

void Nexus::carriage_return() {
    cursor_.col = 0;
}

void Nexus::scroll_up(uint32_t lines) {
    lines = std::min(lines, rows_);

    // Save scrolled lines to scrollback
    for (uint32_t i = 0; i < lines; ++i) {
        ScrollbackLine sbl;
        sbl.chars.resize(cols_);
        sbl.width = cols_;
        for (uint32_t c = 0; c < cols_; ++c) {
            sbl.chars[c] = grid_[i * cols_ + c];
        }
        scrollback_.push_back(std::move(sbl));
        if (scrollback_.size() > MAX_SCROLLBACK) {
            scrollback_.pop_front();
        }
    }

    // Shift grid up
    std::move(grid_.begin() + static_cast<ptrdiff_t>(lines * cols_),
              grid_.end(),
              grid_.begin());

    // Clear new lines at bottom
    for (uint32_t i = rows_ - lines; i < rows_; ++i) {
        for (uint32_t c = 0; c < cols_; ++c) {
            auto& cell = grid_[idx(i, c)];
            cell = ScreenChar{};
            cell.dirty = true;
        }
    }

    if (cursor_.row >= lines) {
        cursor_.row -= lines;
    } else {
        cursor_.row = 0;
    }
}

void Nexus::tab() {
    uint32_t tab_stop = 8;
    cursor_.col = ((cursor_.col + tab_stop) / tab_stop) * tab_stop;
    if (cursor_.col >= cols_) {
        cursor_.col = cols_ - 1;
    }
}

void Nexus::backspace() {
    if (cursor_.col > 0) cursor_.col--;
}

void Nexus::dispatch_escape(char c) {
    switch (c) {
    case 'c': // RIS - reset
        fg_ = Color{255, 255, 255};
        bg_ = Color{0, 0, 0};
        flags_ = CellFlags::None;
        cursor_ = CursorPosition{0, 0};
        for (auto& cell : grid_) {
            cell = ScreenChar{};
            cell.dirty = true;
        }
        break;
    case 'D': // IND - index (same as newline)
        new_line();
        break;
    case 'M': // RI - reverse index
        if (cursor_.row == 0) {
            // Insert blank line at top, scroll down
            std::move_backward(grid_.begin(),
                               grid_.end() - static_cast<ptrdiff_t>(cols_),
                               grid_.end());
            for (uint32_t c = 0; c < cols_; ++c) {
                grid_[c] = ScreenChar{};
                grid_[c].dirty = true;
            }
        } else {
            cursor_.row--;
        }
        break;
    case 'E': // NEL - next line
        new_line();
        break;
    case 'H': // HTS - set tab stop (ignored)
        break;
    case '7': // DECSC - save cursor position
        break;
    case '8': // DECRC - restore cursor position
        break;
    case '(':
    case ')':
    case '*':
    case '+':
        // Designate character set - ignore
        break;
    default:
        break;
    }
}

void Nexus::dispatch_csi() {
    std::array<int, 16> p{};
    for (int i = 0; i < csi_.param_count; ++i) p[i] = csi_.params[i];
    if (csi_.param_count == 0) p[0] = 0;

    char final_byte = csi_.final_byte;

    switch (final_byte) {
    case '@': { // ICH - insert characters
        int n = std::max(p[0], 1);
        n = std::min(n, static_cast<int>(cols_ - cursor_.col));
        for (uint32_t c = cols_ - 1; c >= cursor_.col + static_cast<uint32_t>(n); --c) {
            grid_[idx(cursor_.row, c)] = grid_[idx(cursor_.row, c - n)];
        }
        for (int i = 0; i < n; ++i) {
            grid_[idx(cursor_.row, cursor_.col + i)] = ScreenChar{};
            grid_[idx(cursor_.row, cursor_.col + i)].dirty = true;
        }
        break;
    }
    case 'A': // CUU - cursor up
        cursor_.row = (cursor_.row >= static_cast<uint32_t>(std::max(p[0], 1)))
            ? cursor_.row - std::max(p[0], 1) : 0;
        break;
    case 'B': // CUD - cursor down
        cursor_.row = std::min(cursor_.row + static_cast<uint32_t>(std::max(p[0], 1)), rows_ - 1);
        break;
    case 'C': // CUF - cursor forward
        cursor_.col = std::min(cursor_.col + static_cast<uint32_t>(std::max(p[0], 1)), cols_ - 1);
        break;
    case 'D': // CUB - cursor back
        cursor_.col = (cursor_.col >= static_cast<uint32_t>(std::max(p[0], 1)))
            ? cursor_.col - std::max(p[0], 1) : 0;
        break;
    case 'E': // CNL - cursor next line
        cursor_.row = std::min(cursor_.row + static_cast<uint32_t>(std::max(p[0], 1)), rows_ - 1);
        cursor_.col = 0;
        break;
    case 'F': // CPL - cursor previous line
        cursor_.row = (cursor_.row >= static_cast<uint32_t>(std::max(p[0], 1)))
            ? cursor_.row - std::max(p[0], 1) : 0;
        cursor_.col = 0;
        break;
    case 'G': // CHA - cursor horizontal absolute
        cursor_.col = std::min(static_cast<uint32_t>(std::max(p[0], 1)) - 1, cols_ - 1);
        break;
    case 'H': // CUP - cursor position
    case 'f': // HVP - horizontal vertical position
        set_cursor(p[0] > 0 ? static_cast<uint32_t>(p[0]) : 1,
                   p[1] > 0 ? static_cast<uint32_t>(p[1]) : 1);
        break;
    case 'J': // ED - erase in display
        erase_in_display(p[0]);
        break;
    case 'K': // EL - erase in line
        erase_in_line(p[0]);
        break;
    case 'L': // IL - insert lines
        {
            int n = std::max(p[0], 1);
            n = std::min(n, static_cast<int>(rows_ - cursor_.row));
            std::move_backward(grid_.begin() + static_cast<ptrdiff_t>(idx(cursor_.row, 0)),
                              grid_.end() - static_cast<ptrdiff_t>(n * cols_),
                              grid_.end());
            for (int i = 0; i < n; ++i) {
                for (uint32_t c = 0; c < cols_; ++c) {
                    grid_[idx(cursor_.row + i, c)] = ScreenChar{};
                    grid_[idx(cursor_.row + i, c)].dirty = true;
                }
            }
        }
        break;
    case 'M': // DL - delete lines
        {
            int n = std::max(p[0], 1);
            n = std::min(n, static_cast<int>(rows_ - cursor_.row));
            std::move(grid_.begin() + static_cast<ptrdiff_t>(idx(cursor_.row + n, 0)),
                      grid_.end(),
                      grid_.begin() + static_cast<ptrdiff_t>(idx(cursor_.row, 0)));
            for (uint32_t i = rows_ - static_cast<uint32_t>(n); i < rows_; ++i) {
                for (uint32_t c = 0; c < cols_; ++c) {
                    grid_[idx(i, c)] = ScreenChar{};
                    grid_[idx(i, c)].dirty = true;
                }
            }
        }
        break;
    case 'P': // DCH - delete characters
        {
            int n = std::max(p[0], 1);
            n = std::min(n, static_cast<int>(cols_ - cursor_.col));
            auto start = cursor_.col;
            for (uint32_t c = start; c + static_cast<uint32_t>(n) < cols_; ++c) {
                grid_[idx(cursor_.row, c)] = grid_[idx(cursor_.row, c + n)];
            }
            for (uint32_t c = cols_ - static_cast<uint32_t>(n); c < cols_; ++c) {
                grid_[idx(cursor_.row, c)] = ScreenChar{};
                grid_[idx(cursor_.row, c)].dirty = true;
            }
        }
        break;
    case 'X': // ECH - erase characters
        {
            int n = std::max(p[0], 1);
            n = std::min(n, static_cast<int>(cols_ - cursor_.col));
            for (int i = 0; i < n; ++i) {
                grid_[idx(cursor_.row, cursor_.col + i)] = ScreenChar{};
                grid_[idx(cursor_.row, cursor_.col + i)].dirty = true;
            }
        }
        break;
    case 'm': // SGR - select graphics rendition
        set_graphics_mode(p[0]);
        break;
    case 's': // SCOSC - save cursor position (ignored)
        break;
    case 'u': // SCORC - restore cursor position (ignored)
        break;
    case 'h': // DECSET / SM
    case 'l': // DECRST / RM
        // Handle common modes
        if (p[0] == 25) { // Show/Hide cursor
            cursor_visible_ = (final_byte == 'h');
        }
        break;
    case 'r': // DECSTBM - set scroll region (ignored for now)
        break;
    default:
        break;
    }
}

void Nexus::set_cursor(uint32_t row, uint32_t col) {
    cursor_.row = std::min(row - 1, rows_ - 1);
    cursor_.col = std::min(col - 1, cols_ - 1);
}

void Nexus::set_graphics_mode(int param) {
    if (csi_.param_count > 1) {
        // Process all params
        for (int i = 0; i < csi_.param_count; ++i) {
            set_graphics_mode(csi_.params[i]);
        }
        return;
    }

    if (param >= 30 && param <= 37) {
        static constexpr Color ansi_colors[8] = {
            {0, 0, 0},       // 30 black
            {170, 0, 0},     // 31 red
            {0, 170, 0},     // 32 green
            {170, 85, 0},    // 33 yellow
            {0, 0, 170},     // 34 blue
            {170, 0, 170},   // 35 magenta
            {0, 170, 170},   // 36 cyan
            {170, 170, 170}, // 37 white
        };
        fg_ = ansi_colors[param - 30];
    } else if (param >= 40 && param <= 47) {
        static constexpr Color ansi_bg_colors[8] = {
            {0, 0, 0},
            {170, 0, 0},
            {0, 170, 0},
            {170, 85, 0},
            {0, 0, 170},
            {170, 0, 170},
            {0, 170, 170},
            {170, 170, 170},
        };
        bg_ = ansi_bg_colors[param - 40];
    } else if (param >= 90 && param <= 97) {
        static constexpr Color ansi_bright[8] = {
            {85, 85, 85},
            {255, 85, 85},
            {85, 255, 85},
            {255, 255, 85},
            {85, 85, 255},
            {255, 85, 255},
            {85, 255, 255},
            {255, 255, 255},
        };
        fg_ = ansi_bright[param - 90];
    } else if (param >= 100 && param <= 107) {
        static constexpr Color ansi_bright_bg[8] = {
            {85, 85, 85},
            {255, 85, 85},
            {85, 255, 85},
            {255, 255, 85},
            {85, 85, 255},
            {255, 85, 255},
            {85, 255, 255},
            {255, 255, 255},
        };
        bg_ = ansi_bright_bg[param - 100];
    } else if (param == 38 || param == 48) {
        // 256-color or truecolor - for simplicity, just use basic colors
    } else if (param == 0) {
        fg_ = Color{255, 255, 255};
        bg_ = Color{0, 0, 0};
        flags_ = CellFlags::None;
    } else if (param == 1) {
        flags_ |= CellFlags::Bold;
    } else if (param == 2) {
        flags_ |= CellFlags::Dim;
    } else if (param == 3) {
        flags_ |= CellFlags::Italic;
    } else if (param == 4) {
        flags_ |= CellFlags::Underline;
    } else if (param == 5 || param == 6) {
        flags_ |= CellFlags::Blink;
    } else if (param == 7) {
        flags_ |= CellFlags::Reverse;
    } else if (param == 8) {
        flags_ |= CellFlags::Hidden;
    } else if (param == 9) {
        flags_ |= CellFlags::Strike;
    } else if (param >= 21 && param <= 29) {
        // Reset specific attributes
        CellFlags reset = CellFlags::None;
        switch (param) {
        case 21: case 22: reset = CellFlags::Bold | CellFlags::Dim; break;
        case 23: reset = CellFlags::Italic; break;
        case 24: reset = CellFlags::Underline; break;
        case 25: case 26: reset = CellFlags::Blink; break;
        case 27: reset = CellFlags::Reverse; break;
        case 28: reset = CellFlags::Hidden; break;
        case 29: reset = CellFlags::Strike; break;
        }
        flags_ = flags_ & static_cast<CellFlags>(~static_cast<uint8_t>(reset));
    }
}

void Nexus::erase_in_display(int mode) {
    switch (mode) {
    case 0: // Erase from cursor to end
        for (uint32_t c = cursor_.col; c < cols_; ++c)
            grid_[idx(cursor_.row, c)] = ScreenChar{};
        for (uint32_t r = cursor_.row + 1; r < rows_; ++r)
            for (uint32_t c = 0; c < cols_; ++c)
                grid_[idx(r, c)] = ScreenChar{};
        break;
    case 1: // Erase from beginning to cursor
        for (uint32_t c = 0; c <= cursor_.col; ++c)
            grid_[idx(cursor_.row, c)] = ScreenChar{};
        for (uint32_t r = 0; r < cursor_.row; ++r)
            for (uint32_t c = 0; c < cols_; ++c)
                grid_[idx(r, c)] = ScreenChar{};
        break;
    case 2: // Erase entire display
    case 3: // Erase and scrollback
        for (auto& cell : grid_) cell = ScreenChar{};
        scrollback_.clear();
        break;
    }
    mark_all_dirty();
}

void Nexus::erase_in_line(int mode) {
    switch (mode) {
    case 0: // Erase from cursor to end of line
        for (uint32_t c = cursor_.col; c < cols_; ++c) {
            grid_[idx(cursor_.row, c)] = ScreenChar{};
            grid_[idx(cursor_.row, c)].dirty = true;
        }
        break;
    case 1: // Erase from beginning of line to cursor
        for (uint32_t c = 0; c <= cursor_.col; ++c) {
            grid_[idx(cursor_.row, c)] = ScreenChar{};
            grid_[idx(cursor_.row, c)].dirty = true;
        }
        break;
    case 2: // Erase entire line
        for (uint32_t c = 0; c < cols_; ++c) {
            grid_[idx(cursor_.row, c)] = ScreenChar{};
            grid_[idx(cursor_.row, c)].dirty = true;
        }
        break;
    }
}

void Nexus::osc_dispatch() {
    // Parse OSC sequence
    if (osc_.data.empty()) return;

    // Find the first semicolon for the OSC command number
    auto it = std::find(osc_.data.begin(), osc_.data.end(), ';');
    if (it == osc_.data.end()) return;

    int cmd = 0;
    for (auto p = osc_.data.begin(); p != it; ++p) {
        cmd = cmd * 10 + (*p - '0');
    }

    std::string value(it + 1, osc_.data.end());

    switch (cmd) {
    case 0: // Set icon name + title
    case 2: // Set window title
        title_ = value;
        if (title_cb_) title_cb_(title_);
        break;
    case 1: // Set icon name (ignore)
    case 7: // Set cwd (ignore)
        break;
    default:
        break;
    }
}

} // namespace Kaelum
