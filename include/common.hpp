#pragma once

#include <cstdint>
#include <string_view>
#include <vector>
#include <cstddef>

namespace Kaelum {

struct Color {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    bool operator==(const Color& o) const noexcept = default;
};

enum class CellFlags : uint8_t {
    None      = 0,
    Bold      = 1 << 0,
    Dim       = 1 << 1,
    Italic    = 1 << 2,
    Underline = 1 << 3,
    Blink     = 1 << 4,
    Reverse   = 1 << 5,
    Hidden    = 1 << 6,
    Strike    = 1 << 7,
};

constexpr CellFlags operator|(CellFlags a, CellFlags b) {
    return static_cast<CellFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr CellFlags operator&(CellFlags a, CellFlags b) {
    return static_cast<CellFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr CellFlags& operator|=(CellFlags& a, CellFlags b) {
    return a = a | b;
}

struct Cell {
    char32_t codepoint = U' ';
    Color fg{255, 255, 255};
    Color bg{0, 0, 0};
    CellFlags flags = CellFlags::None;
};

struct CursorPosition {
    uint32_t row = 0;
    uint32_t col = 0;
};

struct CellSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct GridSize {
    uint32_t cols = 0;
    uint32_t rows = 0;
};

struct ScreenSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace Kaelum