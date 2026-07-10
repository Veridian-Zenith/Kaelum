#include "glyph_engine.hpp"
#include "logger.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <unicode/utf8.h>
#include <unistd.h>
#include <fcntl.h>

namespace Kaelum {

GlyphEngine::GlyphEngine() {
    if (FT_Init_FreeType(&ft_lib_) != 0) {
        KAELUM_ERROR("Failed to initialize FreeType");
    }
}

GlyphEngine::~GlyphEngine() {
    if (ft_face_) FT_Done_Face(ft_face_);
    if (ft_lib_) FT_Done_FreeType(ft_lib_);
}

Kaelum::Expected<void, std::string> GlyphEngine::load_font(const std::string& family) {
    if (!ft_lib_) return Kaelum::make_unexpected("FreeType not initialized");

    std::string font_path;
    if (family.empty() || family == "monospace") {
        // Try common monospace fonts
        const char* fonts[] = {
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/TTF/Hack-Regular.ttf",
            "/usr/share/fonts/truetype/hack/Hack-Regular.ttf",
            "/usr/share/fonts/TTF/FiraCode-Regular.ttf",
            "/usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf",
            "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
            nullptr
        };
        for (const char** f = fonts; *f; ++f) {
            if (access(*f, R_OK) == 0) {
                font_path = *f;
                break;
            }
        }
    } else {
        font_path = family;
    }

    if (font_path.empty()) {
        return Kaelum::make_unexpected("No font found");
    }

    if (ft_face_) FT_Done_Face(ft_face_);
    
    if (FT_New_Face(ft_lib_, font_path.c_str(), 0, &ft_face_) != 0) {
        return Kaelum::make_unexpected("Failed to load font: " + font_path);
    }

    // Set pixel size
    if (FT_Set_Pixel_Sizes(ft_face_, 0, 16) != 0) {
        return Kaelum::make_unexpected("Failed to set pixel size");
    }

    cell_width_ = ft_face_->size->metrics.max_advance >> 6;
    line_height_ = (ft_face_->size->metrics.height >> 6) + 2;

    // Initialize atlas
    atlas_data_.assign(atlas_width_ * atlas_height_, 0);
    
    KAELUM_INFO("Font loaded: {} ({}x{})", font_path, cell_width_, line_height_);
    return {};
}

const AtlasGlyph* GlyphEngine::get_glyph(char32_t codepoint) {
    auto it = glyph_cache_.find(codepoint);
    if (it != glyph_cache_.end()) return &it->second;
    return nullptr;
}

Kaelum::Expected<AtlasGlyph, std::string> GlyphEngine::cache_glyph(char32_t codepoint) {
    if (!ft_face_) return Kaelum::Expected<AtlasGlyph, std::string>("Font not loaded");

    FT_UInt glyph_index = FT_Get_Char_Index(ft_face_, codepoint);
    if (glyph_index == 0) {
        // Missing glyph
        AtlasGlyph g{0, 0, 0, 0, 0, 0, 0};
        glyph_cache_[codepoint] = g;
        return g;
    }

    if (FT_Load_Glyph(ft_face_, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
        return Kaelum::Expected<AtlasGlyph, std::string>("Failed to load glyph");
    }

    FT_Bitmap& bitmap = ft_face_->glyph->bitmap;
    AtlasGlyph glyph;
    glyph.advance = ft_face_->glyph->advance.x >> 6;
    glyph.x_offset = ft_face_->glyph->bitmap_left;
    glyph.y_offset = -ft_face_->glyph->bitmap_top;

    // Check if we need more space in atlas
    if (atlas_next_x_ + bitmap.width + 1 >= atlas_width_) {
        atlas_next_x_ = 0;
        atlas_next_y_ += atlas_row_height_ + 1;
        atlas_row_height_ = 0;
    }
    if (atlas_next_y_ + bitmap.rows + 1 >= atlas_height_) {
        return Kaelum::Expected<AtlasGlyph, std::string>("Atlas full");
    }

    // Pack glyph into atlas
    glyph.u0 = atlas_next_x_;
    glyph.v0 = atlas_next_y_;
    glyph.u1 = atlas_next_x_ + bitmap.width;
    glyph.v1 = atlas_next_y_ + bitmap.rows;

    // Copy bitmap data to atlas
    for (int y = 0; y < bitmap.rows; ++y) {
        for (int x = 0; x < bitmap.width; ++x) {
            size_t src_idx = y * bitmap.pitch + x;
            size_t dst_idx = (atlas_next_y_ + y) * atlas_width_ + atlas_next_x_ + x;
            if (dst_idx < atlas_data_.size()) {
                atlas_data_[dst_idx] = bitmap.buffer[src_idx];
            }
        }
    }

    atlas_next_x_ += bitmap.width + 1;
    if (bitmap.rows > atlas_row_height_) atlas_row_height_ = bitmap.rows;
    atlas_dirty_ = true;

    glyph_cache_[codepoint] = glyph;
    return glyph;
}

} // namespace Kaelum