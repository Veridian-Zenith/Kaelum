#pragma once

#include "common.hpp"
#include "expected.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <unordered_map>
#include <string>
#include <vector>

namespace Kaelum {

struct AtlasGlyph {
    uint32_t u0, v0, u1, v1;
    int32_t x_offset, y_offset;
    uint32_t advance;
};

class GlyphEngine {
public:
    GlyphEngine();
    ~GlyphEngine();

    GlyphEngine(const GlyphEngine&) = delete;
    GlyphEngine& operator=(const GlyphEngine&) = delete;
    GlyphEngine(GlyphEngine&&) = delete;
    GlyphEngine& operator=(GlyphEngine&&) = delete;

    Kaelum::Expected<void, std::string> load_font(const std::string& family = "");
    const AtlasGlyph* get_glyph(char32_t codepoint);
    Kaelum::Expected<AtlasGlyph, std::string> cache_glyph(char32_t codepoint);
    
    uint32_t cell_width() const { return cell_width_; }
    uint32_t line_height() const { return line_height_; }
    uint32_t atlas_width() const { return atlas_width_; }
    uint32_t atlas_height() const { return atlas_height_; }
    const std::vector<uint8_t>& atlas_data() const { return atlas_data_; }
    bool atlas_dirty() const { return atlas_dirty_; }
    void clear_atlas_dirty() const { atlas_dirty_ = false; }

private:
    FT_Library ft_lib_ = nullptr;
    FT_Face ft_face_ = nullptr;
    std::string font_family_;
    
    uint32_t cell_width_ = 0;
    uint32_t line_height_ = 0;
    
    uint32_t atlas_width_ = 1024;
    uint32_t atlas_height_ = 1024;
    std::vector<uint8_t> atlas_data_;
    uint32_t atlas_next_x_ = 0;
    uint32_t atlas_next_y_ = 0;
    uint32_t atlas_row_height_ = 0;
    mutable bool atlas_dirty_ = false;
    
    std::unordered_map<char32_t, AtlasGlyph> glyph_cache_;
    
    bool pack_glyph(FT_Bitmap bitmap, AtlasGlyph& out);
    void ensure_atlas_space(uint32_t w, uint32_t h);
    void upload_to_atlas(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* data);
};

} // namespace Kaelum