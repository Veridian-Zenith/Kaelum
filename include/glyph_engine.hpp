#pragma once

#include <string>
#include <vector>
#include <map>
#include <expected>
#include <cstdint>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

namespace Kaelum {

    struct GlyphMetric {
        uint32_t bearing_x;
        uint32_t bearing_y;
        uint32_t width;
        uint32_t height;
        uint32_t advance;
    };

    struct GlyphData {
        GlyphMetric metric;
        std::vector<uint8_t> bitmap;
    };

    enum class GlyphError {
        FreeTypeInitFailed,
        FontLoadFailed,
        GlyphLoadFailed
    };

    class GlyphEngine {
    public:
        GlyphEngine();
        ~GlyphEngine();

        std::expected<void, GlyphError> load_font(const std::string& font_family = "Monospace");
        std::expected<GlyphData, GlyphError> get_glyph(char32_t codepoint);
        
        uint32_t get_line_height() const { return line_height_; }
        uint32_t get_cell_width() const { return cell_width_; }

    private:
        FT_Library library_ = nullptr;
        FT_Face face_ = nullptr;
        uint32_t line_height_ = 0;
        uint32_t cell_width_ = 0;
        std::map<char32_t, GlyphData> glyph_cache_;
    };

} // namespace Kaelum
