#include "nexus.hpp"

namespace Kaelum {

Nexus::Nexus() {
    // Initialize grid with default cells
    grid_.fill(Cell{});
}

std::expected<void, NexusError> Nexus::process_input(std::span<const uint8_t> data) {
    // Minimal implementation: just put chars in the grid
    for (auto byte : data) {
        if (cursor_x_ < k_default_cols) {
            grid_[cursor_y_ * k_default_cols + cursor_x_].codepoint = static_cast<char32_t>(byte);
            cursor_x_++;
        } else {
            cursor_x_ = 0;
            cursor_y_ = (cursor_y_ + 1) % k_default_rows;
        }
    }
    return {};
}

} // namespace Kaelum
