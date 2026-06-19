#include "glyph_engine.hpp"
#include <print>
#include <cstring>

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

std::expected<void, GlyphError> GlyphEngine::load_font(const std::string& font_family) {
    // We can skip explicit config handling and use the default match
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(font_family.c_str()));
    if (!pattern) {
        std::println(stderr, "GlyphEngine: Failed to parse font name");
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    FcResult result;
    FcPattern* matched_pattern = FcFontMatch(nullptr, pattern, &result);
    if (!matched_pattern) {
        std::println(stderr, "GlyphEngine: Could not find match for font family {}", font_family);
        FcPatternDestroy(pattern);
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    FcChar8* font_path = nullptr;
    if (FcPatternGetString(matched_pattern, FC_FILE, 0, &font_path) == FcResultMatch) {
        // Success
    } else {
        std::println(stderr, "GlyphEngine: Could not get font path from matched pattern");
        FcPatternDestroy(pattern);
        FcPatternDestroy(matched_pattern);
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    if (FT_New_Face(library_, reinterpret_cast<const char*>(font_path), 0, &face_)) {
        std::println(stderr, "GlyphEngine: Failed to load font face from {}", reinterpret_cast<const char*>(font_path));
        FcPatternDestroy(pattern);
        FcPatternDestroy(matched_pattern);
        return std::unexpected(GlyphError::FontLoadFailed);
    }

    FT_Set_Pixel_Sizes(face_, 0, 14);
    line_height_ = static_cast<uint32_t>(face_->size->metrics.height >> 6);
    cell_width_ = static_cast<uint32_t>(face_->size->metrics.max_advance >> 6);

    std::println("GlyphEngine: Loaded font face: {}", reinterpret_cast<const char*>(face_->family_name));

    FcPatternDestroy(pattern);
    FcPatternDestroy(matched_pattern);

    return {};
}

std::expected<GlyphData, GlyphError> GlyphEngine::get_glyph(char32_t codepoint) {
    if (glyph_cache_.contains(codepoint)) {
        return glyph_cache_[codepoint];
    }

    if (FT_Load_Char(face_, codepoint, FT_LOAD_RENDER)) {
        std::println(stderr, "GlyphEngine: Failed to load glyph for codepoint {}", static_cast<uint32_t>(codepoint));
        return std::unexpected(GlyphError::GlyphLoadFailed);
    }

    FT_GlyphSlot slot = face_->glyph;
    GlyphData data;
    data.metric = {
        .bearing_x = static_cast<uint32_t>(slot->bitmap_left),
        .bearing_y = static_cast<uint32_t>(slot->bitmap_top),
        .width = static_cast<uint32_t>(slot->bitmap.width),
        .height = static_cast<uint32_t>(slot->bitmap.rows),
        .advance = static_cast<uint32_t>(slot->advance.x >> 6)
    };

    size_t buffer_size = slot->bitmap.width * slot->bitmap.rows;
    data.bitmap.resize(buffer_size);
    if (buffer_size > 0) {
        std::memcpy(data.bitmap.data(), slot->bitmap.buffer, buffer_size);
    }

    glyph_cache_[codepoint] = data;
    return data;
}

} // namespace Kaelum
