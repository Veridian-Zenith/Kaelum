#pragma once

#include <expected>
#include <memory>
#include <vector>
#include <functional>
#include <wayland-client.h>
#include <vulkan/vulkan.h>
#include "common.hpp"
#include "nexus.hpp"
#include "glyph_engine.hpp"

struct SigilVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

struct GlyphRect {
    float u0, v0, u1, v1;
    // Pixel-space metrics for positioning within a cell
    int32_t bearing_x, bearing_y;
    uint32_t width, height, advance;
