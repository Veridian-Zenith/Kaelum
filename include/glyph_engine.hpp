#pragma once

#include <string>
#include <vector>
#include <map>
#include <expected>
#include <cstdint>
#include <span>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

namespace Kaelum {

struct GlyphMetric {
    int32_t bearing_x;
    int32_t bearing_y;
    uint32_t width;
    uint32_t height;
    uint32_t advance;
    float u0, v0, u1, v1;
};

struct GlyphBitmap {
    std::vector<uint8_t> pixels;
    uint32_t width;
    uint32_t height;
    int32_t bearing_x;
    int32_t bearing_y;
    uint32_t advance;
};

struct AtlasGlyph {
    float u0, v0, u1, v1;
    int32_t bearing_x, bearing_y;
    uint32_t width, height, advance;
};

enum class GlyphError {
    FontLoadFailed,
    GlyphNotFound,
    AtlasFull,
    FreeTypeError,
    FontConfigError,
};

class GlyphEngine {
public:
    GlyphEngine();
    ~GlyphEngine();

    GlyphEngine(const GlyphEngine&) = delete;
    GlyphEngine& operator=(const GlyphEngine&) = delete;
    GlyphEngine(GlyphEngine&&) = delete;
    GlyphEngine& operator=(GlyphEngine&&) = delete;

    std::expected<void, GlyphError> load_font(const std::string& font_family = "monospace");
    std::expected<GlyphBitmap, GlyphError> render_codepoint(char32_t cp);

    const AtlasGlyph* get_glyph(char32_t cp);
    std::expected<AtlasGlyph, GlyphError> cache_glyph(char32_t cp);

    uint32_t line_height() const { return line_height_; }
    uint32_t cell_width() const { return cell_width_; }
    uint32_t font_size() const { return font_size_; }
    void set_font_size(uint32_t size);

    uint32_t atlas_width() const { return atlas_width_; }
    uint32_t atlas_height() const { return atlas_height_; }
    const std::vector<uint8_t>& atlas_data() const { return atlas_data_; }
    bool atlas_dirty() const { return atlas_dirty_; }
    void clear_atlas_dirty() { atlas_dirty_ = false; }

    const std::map<char32_t, AtlasGlyph>& glyphs() const { return glyphs_; }

private:
    FT_Library library_ = nullptr;
    FT_Face face_ = nullptr;
    uint32_t font_size_ = 14;
    uint32_t line_height_ = 0;
    uint32_t cell_width_ = 0;

    uint32_t atlas_width_ = 1024;
    uint32_t atlas_height_ = 1024;
    std::vector<uint8_t> atlas_data_;
    uint32_t atlas_cursor_x_ = 1;
    uint32_t atlas_cursor_y_ = 1;
    uint32_t atlas_row_height_ = 0;
    bool atlas_dirty_ = false;

    std::map<char32_t, AtlasGlyph> glyphs_;

    std::expected<AtlasGlyph, GlyphError> pack_into_atlas(const GlyphBitmap& bitmap);
    void grow_atlas();
};

} // namespace Kaelum
