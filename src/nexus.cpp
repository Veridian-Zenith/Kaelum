#include "nexus.hpp"
#include <iostream>
#include <algorithm>
#include <charconv>

namespace Kaelum {

Nexus::Nexus() {
    grid_.fill(Cell{});
    
    // O(1) Dispatch table using member function pointers
    dispatch_table_[static_cast<size_t>(State::Ground)] = &Nexus::handle_ground;
    dispatch_table_[static_cast<size_t>(State::Escape)] = &Nexus::handle_escape;
    dispatch_table_[static_cast<size_t>(State::CSI)]    = &Nexus::handle_csi;
    dispatch_table_[static_cast<size_t>(State::OSC)]    = &Nexus::handle_osc;
}

void Nexus::handle_ground(uint8_t c) {
    if (c == 27) {
        current_state_ = State::Escape;
    } else if (c == '\n') {
        cursor_y_ = (cursor_y_ + 1) % k_default_rows;
        cursor_x_ = 0;
    } else if (c == '\r') {
        cursor_x_ = 0;
    } else if (c == '\t') {
        cursor_x_ = (cursor_x_ + 8) % k_default_cols;
    } else {
        set_cell(static_cast<char32_t>(c), {255, 255, 255, 255}, {0, 0, 0, 255}, 0);
        cursor_x_ = (cursor_x_ + 1) % k_default_cols;
        if (cursor_x_ == 0) {
            cursor_y_ = (cursor_y_ + 1) % k_default_rows;
        }
    }
}

void Nexus::handle_escape(uint8_t c) {
    if (c == '[') {
        current_state_ = State::CSI;
        sequence_buffer_.clear();
    } else if (c == ']') {
        current_state_ = State::OSC;
        sequence_buffer_.clear();
    } else {
        // Not a valid sequence, treat as normal text (ESC + char)
        handle_ground(27);
        handle_ground(c);
        current_state_ = State::Ground;
    }
}

void Nexus::handle_csi(uint8_t c) {
    if (std::isdigit(c) || c == ';') {
        sequence_buffer_.push_back(c);
    } else {
        process_csi(c);
        sequence_buffer_.clear();
        current_state_ = State::Ground;
    }
}

void Nexus::process_csi(uint8_t final_char) {
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
        case 'H': // Cursor Position (CUP)
            {
                int row = params.size() > 0 ? params[0] : 1;
                int col = params.size() > 1 ? params[1] : 1;
                cursor_x_ = std::clamp(col - 1, 0, (int)k_default_cols - 1);
                cursor_y_ = std::clamp(row - 1, 0, (int)k_default_rows - 1);
            }
            break;
        case 'f': // Cursor Position (CUP)
            // Same as 'H'
            break;
        case 'J': // Erase in Display (ED)
            {
                int mode = params.empty() ? 0 : params[0];
                if (mode == 0 || mode == 2) clear_screen();
            }
            break;
        case 'm': // Select Graphic Rendition (SGR)
            parse_sgr(sequence_buffer_);
            break;
        default:
            break;
    }
}

void Nexus::parse_sgr(std::span<const uint8_t> params) {
    // Simplified SGR parsing for colors
    // In a real implementation, this would handle 256-color and TrueColor
}

void Nexus::move_cursor(int dx, int dy) {
    cursor_x_ = std::clamp(static_cast<size_t>(cursor_x_ + dx), size_t(0), k_default_cols - 1);
    cursor_y_ = std::clamp(static_cast<size_t>(cursor_y_ + dy), size_t(0), k_default_rows - 1);
}

void Nexus::set_cell(char32_t cp, Color fg, Color bg, uint32_t attrs) {
    grid_[cursor_y_ * k_default_cols + cursor_x_] = Cell{cp, fg, bg, attrs};
}

void Nexus::clear_screen() {
    grid_.fill(Cell{});
    cursor_x_ = 0;
    cursor_y_ = 0;
}

void Nexus::handle_osc(uint8_t c) {
    if (c == '\a' || c == '\n') {
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
