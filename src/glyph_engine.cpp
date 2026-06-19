#include "glyph_engine.hpp"
=======
#include <iostream>
>>>>>>> f3a208535ac134d49e379d14c6e49e33196c5e79
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
<<<<<<< HEAD
    cell_width_ = static_cast<uint32_t>(face_->size->metrics.max_advance >> 6);
