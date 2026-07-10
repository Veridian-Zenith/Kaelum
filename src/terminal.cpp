#include "terminal.hpp"
#include "logger.hpp"

namespace Kaelum {

void Terminal::set_size(uint32_t cols, uint32_t rows) {
    KAELUM_DEBUG("Terminal resize to {}x{}", cols, rows);
    grid_size_ = {cols, rows};
    grid_.resize(cols * rows);
    dirty_.assign(cols * rows, true);
    cursor_ = {0, 0};
}

void Terminal::reset() {
    std::fill(grid_.begin(), grid_.end(), Cell{});
    std::fill(dirty_.begin(), dirty_.end(), true);
    cursor_ = {0, 0};
    state_ = State::Ground;
    csi_params_.clear();
    osc_buffer_.clear();
}

void Terminal::process_bytes(std::span<const char> data) {
    for (unsigned char c : data) {
        process_char(c);
    }
}

void Terminal::process_char(char c) {
    switch (state_) {
        case State::Ground:
            if (c < 0x20) {
                handle_control(c);
            } else if (c == 0x1B) {
                state_ = State::Escape;
            } else {
                handle_char(static_cast<char32_t>(c));
            }
            break;
        case State::Escape:
            handle_escape(c);
            break;
        case State::CsiEntry:
            if (c >= '0' && c <= '9') {
                csi_params_.push_back(c - '0');
                state_ = State::CsiParam;
            } else if (c == ';') {
                csi_params_.push_back(-1);
                state_ = State::CsiParam;
            } else if (c >= 0x30 && c <= 0x3F) {
                csi_params_.push_back(-1);
                state_ = State::CsiIntermediate;
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi(c);
                state_ = State::Ground;
            } else {
                state_ = State::Ground;
            }
            break;
        case State::CsiParam:
            if (c >= '0' && c <= '9') {
                int& last = csi_params_.back();
                if (last == -1) last = c - '0';
                else last = last * 10 + (c - '0');
            } else if (c == ';') {
                csi_params_.push_back(-1);
            } else if (c >= 0x30 && c <= 0x3F) {
                state_ = State::CsiIntermediate;
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi(c);
                state_ = State::Ground;
            } else {
                state_ = State::Ground;
            }
            break;
        case State::CsiIntermediate:
            if (c >= 0x40 && c <= 0x7E) {
                handle_csi(c);
            }
            state_ = State::Ground;
            break;
        case State::OscString:
            if (c == 0x07 || c == 0x1B) {
                handle_osc(c);
                state_ = State::Ground;
            } else {
                osc_buffer_.push_back(c);
            }
            break;
        default:
            state_ = State::Ground;
            break;
    }
}

void Terminal::handle_control(char c) {
    switch (c) {
        case 0x07: // BEL
            break;
        case 0x08: // BS
            if (cursor_.col > 0) cursor_.col--;
            break;
        case 0x09: // HT
            cursor_.col = ((cursor_.col / 8) + 1) * 8;
            if (cursor_.col >= grid_size_.cols) cursor_.col = grid_size_.cols - 1;
            break;
        case 0x0A: // LF
        case 0x0B: // VT
        case 0x0C: // FF
            if (cursor_.row + 1 >= grid_size_.rows) {
                scroll_up();
            } else {
                cursor_.row++;
            }
            break;
        case 0x0D: // CR
            cursor_.col = 0;
            break;
        default:
            break;
    }
}

void Terminal::handle_escape(char c) {
    switch (c) {
        case '[':
            csi_params_.clear();
            state_ = State::CsiEntry;
            break;
        case ']':
            osc_buffer_.clear();
            state_ = State::OscString;
            break;
        case 'c': // RIS
            reset();
            break;
        default:
            state_ = State::Ground;
            break;
    }
}

void Terminal::handle_csi(char c) {
    int param = csi_params_.empty() ? 1 : csi_params_[0];
    if (param < 0) param = 1;

    switch (c) {
        case 'A': // CUU
            if (cursor_.row >= static_cast<uint32_t>(param)) cursor_.row -= param;
            else cursor_.row = 0;
            break;
        case 'B': // CUD
            if (cursor_.row + param < grid_size_.rows) cursor_.row += param;
            else cursor_.row = grid_size_.rows - 1;
            break;
        case 'C': // CUF
            if (cursor_.col + param < grid_size_.cols) cursor_.col += param;
            else cursor_.col = grid_size_.cols - 1;
            break;
        case 'D': // CUB
            if (cursor_.col >= static_cast<uint32_t>(param)) cursor_.col -= param;
            else cursor_.col = 0;
            break;
        case 'H': // CUP
        case 'f': // HVP
            {
                int row = csi_params_.size() > 1 ? csi_params_[1] : 1;
                int col = csi_params_.size() > 0 ? csi_params_[0] : 1;
                if (row < 1) row = 1;
                if (col < 1) col = 1;
                cursor_.row = std::min(static_cast<uint32_t>(row - 1), grid_size_.rows - 1);
                cursor_.col = std::min(static_cast<uint32_t>(col - 1), grid_size_.cols - 1);
            }
            break;
        case 'J': // ED
            // Clear screen
            break;
        case 'K': // EL
            // Clear line
            break;
        case 'm': // SGR
            // Attributes
            break;
        default:
            break;
    }
    state_ = State::Ground;
    csi_params_.clear();
}

void Terminal::handle_osc(char c) {
    // Operating System Command
    osc_buffer_.clear();
    state_ = State::Ground;
}

void Terminal::handle_char(char32_t cp) {
    Cell& cell = grid_[cursor_.row * grid_size_.cols + cursor_.col];
    cell.codepoint = cp;
    mark_dirty(cursor_.row, cursor_.col);

    if (cursor_.col + 1 >= grid_size_.cols) {
        if (cursor_.row + 1 >= grid_size_.rows) {
            scroll_up();
        } else {
            cursor_.row++;
            cursor_.col = 0;
        }
    } else {
        cursor_.col++;
    }
}

void Terminal::scroll_up() {
    size_t cols = grid_size_.cols;
    std::copy(grid_.begin() + cols, grid_.end(), grid_.begin());
    std::fill(grid_.end() - cols, grid_.end(), Cell{});
    std::copy(dirty_.begin() + cols, dirty_.end(), dirty_.begin());
    std::fill(dirty_.end() - cols, dirty_.end(), true);
}

void Terminal::mark_all_dirty() {
    std::fill(dirty_.begin(), dirty_.end(), true);
}

void Terminal::mark_dirty(uint32_t row, uint32_t col) {
    if (row < grid_size_.rows && col < grid_size_.cols) {
        dirty_[row * grid_size_.cols + col] = true;
    }
}

bool Terminal::is_dirty(uint32_t row, uint32_t col) const {
    if (row < grid_size_.rows && col < grid_size_.cols) {
        return dirty_[row * grid_size_.cols + col];
    }
    return false;
}

void Terminal::clear_dirty(uint32_t row, uint32_t col) {
    if (row < grid_size_.rows && col < grid_size_.cols) {
        dirty_[row * grid_size_.cols + col] = false;
    }
}

} // namespace Kaelum