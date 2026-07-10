#include "glyph_engine.hpp"
#include <print>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace Kaelum {

GlyphEngine::GlyphEngine() {
    if (FT_Init_FreeType(&library_)) {
        std::println(stderr, "GlyphEngine: Failed to initialize FreeType");
    }
}

GlyphEngine::~GlyphEngine() {
    if (face_) FT_Done_Face(face_);
    if (library_) FT_Done_FreeType(library_);
}

void GlyphEngine::set_font_size(uint32_t size) {
    if (size == font_size_) return;
    font_size_ = size;
    if (face_) {
        FT_Set_Pixel_Sizes(face_, 0, font_size_);
        line_height_ = static_cast<uint32_t>(face_->size->metrics.height >> 6);
        cell_width_ = static_cast<uint32_t>(face_->size->metrics.max_advance >> 6);
        glyphs_.clear();
        atlas_cursor_x_ = 1;
        atlas_cursor_y_ = 1;
        atlas_row_height_ = 0;
        std::fill(atlas_data_.begin(), atlas_data_.end(), 0);
        atlas_dirty_ = false;
    }
}

std::expected<void, GlyphError> GlyphEngine::load_font(const std::string& font_family) {
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(font_family.c_str()));
    if (!pattern) {
        std::println(stderr, "GlyphEngine: Failed to parse font name");
        return std::unexpected(GlyphError::FontConfigError);
    }

    FcResult result;
    FcPattern* matched_pattern = FcFontMatch(nullptr, pattern, &result);
    if (!matched_pattern) {
        std::println(stderr, "GlyphEngine: Could not find match for font family {}", font_family);
        FcPatternDestroy(pattern);
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    FcChar8* font_path = nullptr;
    if (FcPatternGetString(matched_pattern, FC_FILE, 0, &font_path) != FcResultMatch) {
        std::println(stderr, "GlyphEngine: Could not get font path from matched pattern");
        FcPatternDestroy(pattern);
        FcPatternDestroy(matched_pattern);
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    if (FT_New_Face(library_, reinterpret_cast<const char*>(font_path), 0, &face_)) {
        std::println(stderr, "GlyphEngine: Failed to load font face from {}",
                     reinterpret_cast<const char*>(font_path));
        FcPatternDestroy(pattern);
        FcPatternDestroy(matched_pattern);
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    FT_Set_Pixel_Sizes(face_, 0, font_size_);
    line_height_ = static_cast<uint32_t>(face_->size->metrics.height >> 6);
    cell_width_ = static_cast<uint32_t>(face_->size->metrics.max_advance >> 6);

    if (cell_width_ == 0) cell_width_ = line_height_ / 2;

    FcPatternDestroy(pattern);
    FcPatternDestroy(matched_pattern);

    // Initialize atlas
    atlas_data_.resize(atlas_width_ * atlas_height_, 0);

    return {};
}

std::expected<GlyphBitmap, GlyphError> GlyphEngine::render_codepoint(char32_t cp) {
    if (!face_) {
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    FT_UInt glyph_index = FT_Get_Char_Index(face_, cp);
    if (glyph_index == 0) {
        // Use replacement character
        glyph_index = FT_Get_Char_Index(face_, 0xFFFD);
        if (glyph_index == 0) {
            return std::unexpected(GlyphError::GlyphNotFound);
        }
    }

    if (FT_Load_Glyph(face_, glyph_index, FT_LOAD_RENDER)) {
        return std::unexpected(GlyphError::FreeTypeError);
    }

    FT_Bitmap& bitmap = face_->glyph->bitmap;

    GlyphBitmap result;
    result.width = bitmap.width;
    result.height = bitmap.rows;
    result.bearing_x = face_->glyph->bitmap_left;
    result.bearing_y = face_->glyph->bitmap_top;
    result.advance = static_cast<uint32_t>(face_->glyph->advance.x >> 6);

    if (bitmap.width > 0 && bitmap.rows > 0) {
        result.pixels.resize(bitmap.width * bitmap.rows);

        if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (uint32_t y = 0; y < bitmap.rows; ++y) {
                for (uint32_t x = 0; x < bitmap.width; ++x) {
                    int byte_idx = y * bitmap.pitch + (x / 8);
                    int bit_idx = 7 - (x % 8);
                    result.pixels[y * bitmap.width + x] =
                        (bitmap.buffer[byte_idx] & (1 << bit_idx)) ? 255 : 0;
                }
            }
        } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
            std::memcpy(result.pixels.data(), bitmap.buffer,
                        bitmap.width * bitmap.rows);
        } else {
            std::memset(result.pixels.data(), 255, bitmap.width * bitmap.rows);
        }
    }

    return result;
}

const AtlasGlyph* GlyphEngine::get_glyph(char32_t cp) {
    auto it = glyphs_.find(cp);
    if (it != glyphs_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::expected<AtlasGlyph, GlyphError> GlyphEngine::cache_glyph(char32_t cp) {
    // Check if already cached
    if (auto* existing = get_glyph(cp)) {
        return *existing;
    }

    auto bitmap_res = render_codepoint(cp);
    if (!bitmap_res) {
        return std::unexpected(bitmap_res.error());
    }

    auto atlas_res = pack_into_atlas(*bitmap_res);
    if (atlas_res) {
        glyphs_.emplace(cp, *atlas_res);
    }
    return atlas_res;
}

std::expected<AtlasGlyph, GlyphError> GlyphEngine::pack_into_atlas(const GlyphBitmap& bitmap) {
    uint32_t gw = std::max(bitmap.width, 1u);
    uint32_t gh = std::max(bitmap.height, 1u);

    // Add 1 pixel padding
    gw += 2;
    gh += 2;

    if (atlas_cursor_x_ + gw >= atlas_width_) {
        atlas_cursor_x_ = 1;
        atlas_cursor_y_ += atlas_row_height_ + 1;
        atlas_row_height_ = 0;
    }

    if (atlas_cursor_y_ + gh >= atlas_height_) {
        grow_atlas();
    }

    atlas_row_height_ = std::max(atlas_row_height_, gh);

    // Copy glyph into atlas
    for (uint32_t y = 0; y < bitmap.height; ++y) {
        uint32_t atlas_y = atlas_cursor_y_ + 1 + y;
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            uint32_t atlas_x = atlas_cursor_x_ + 1 + x;
            atlas_data_[atlas_y * atlas_width_ + atlas_x] =
                bitmap.pixels[y * bitmap.width + x];
        }
    }

    AtlasGlyph ag;
    ag.u0 = (static_cast<float>(atlas_cursor_x_ + 1)) / static_cast<float>(atlas_width_);
    ag.v0 = (static_cast<float>(atlas_cursor_y_ + 1)) / static_cast<float>(atlas_height_);
    ag.u1 = (static_cast<float>(atlas_cursor_x_ + 1 + bitmap.width)) / static_cast<float>(atlas_width_);
    ag.v1 = (static_cast<float>(atlas_cursor_y_ + 1 + bitmap.height)) / static_cast<float>(atlas_height_);

    ag.bearing_x = bitmap.bearing_x;
    ag.bearing_y = bitmap.bearing_y;
    ag.width = bitmap.width;
    ag.height = bitmap.height;
    ag.advance = bitmap.advance;

    atlas_cursor_x_ += gw;
    atlas_dirty_ = true;

    return ag;
}

void GlyphEngine::grow_atlas() {
    uint32_t new_w = atlas_width_ * 2;
    uint32_t new_h = atlas_height_ * 2;

    std::vector<uint8_t> new_data(new_w * new_h, 0);

    for (uint32_t y = 0; y < atlas_height_; ++y) {
        std::memcpy(&new_data[y * new_w], &atlas_data_[y * atlas_width_], atlas_width_);
    }

    atlas_width_ = new_w;
    atlas_height_ = new_h;
    atlas_data_ = std::move(new_data);
    atlas_dirty_ = true;

    std::println(stdout, "GlyphEngine: Atlas grown to {}x{}", atlas_width_, atlas_height_);
}

} // namespace Kaelum
