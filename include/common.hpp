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

    // Veridian Zenith "Atmosphere" palette — deep void with amber accents
    constexpr Color vz_bg      = {5, 2, 0, 255};       // --vz-bg-primary  #050200
    constexpr Color vz_fg      = {243, 244, 246, 255};  // body text         #f3f4f6
    constexpr Color vz_accent  = {255, 179, 71, 255};   // --vz-accent-vibrant #FFB347

    /**
     * @brief A single terminal cell
     */
    struct Cell {
        char32_t codepoint = U' ';
        Color fg = vz_fg;
        Color bg = vz_bg;
        uint32_t attrs = 0; // Bold, Italic, Underline, etc.

        constexpr bool operator==(const Cell&) const = default;
    };

    /**
     * @brief Grid of cells representing the terminal state
     */
    using Grid = std::array<Cell, k_default_cols * k_default_rows>;

} // namespace Kaelum
