#pragma once

#include <cstdint>
#include <string_view>
#include <array>

namespace Kaelum {

    /**
     * @brief Constants for the terminal grid
     */
    constexpr size_t k_default_cols = 80;
    constexpr size_t k_default_rows = 24;

    /**
     * @brief Color representation (RGBA8)
     */
    struct Color {
        uint8_t r, g, b, a;

        constexpr bool operator==(const Color&) const = default;
    };

    /**
     * @brief A single terminal cell
     */
    struct Cell {
        char32_t codepoint = U' ';
        Color fg = {255, 255, 255, 255};
        Color bg = {0, 0, 0, 255};
        uint32_t attrs = 0; // Bold, Italic, Underline, etc.

        constexpr bool operator==(const Cell&) const = default;
    };

    /**
     * @brief Grid of cells representing the terminal state
     */
    using Grid = std::array<Cell, k_default_cols * k_default_rows>;

} // namespace Kaelum
