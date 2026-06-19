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
        int32_t bearing_x;
        int32_t bearing_y;
