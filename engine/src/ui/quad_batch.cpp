#include "vxe/ui/quad_batch.hpp"

#include <algorithm>
#include <cmath>

namespace vxe::ui {
namespace {

constexpr u32 kFlagTextured = 1;

// The drop shadow color: the text color at a quarter brightness, same alpha.
constexpr u32 shadowOf(u32 color) noexcept {
    return ((color & 0x00FCFCFCU) >> 2U) | (color & 0xFF000000U);
}

Rect intersect(const Rect& a, const Rect& b) noexcept {
    const f32 left = std::max(a.x, b.x);
    const f32 top = std::max(a.y, b.y);
    const f32 right = std::min(a.x + a.width, b.x + b.width);
    const f32 bottom = std::min(a.y + a.height, b.y + b.height);
    return {
        .x = left, .y = top, .width = std::max(0.0F, right - left), .height = std::max(0.0F, bottom - top)};
}

// The part of a region between fractions of its width and height.
std::array<f32, 4> subRegion(const AtlasRegion& region, f32 fx0, f32 fy0, f32 fx1, f32 fy1) noexcept {
    const f32 du = region.u1 - region.u0;
    const f32 dv = region.v1 - region.v0;
    return {region.u0 + du * fx0, region.v0 + dv * fy0, region.u0 + du * fx1, region.v0 + dv * fy1};
}

} // namespace

const AtlasRegion* UiResources::image(std::string_view name) const noexcept {
    const auto found =
        std::ranges::find(images, name, [](const auto& entry) { return std::string_view(entry.first); });
    return found == images.end() ? nullptr : &found->second;
}

void QuadBatch::reset(const UiResources* resources) noexcept {
    m_resources = resources;
    m_quads.clear();
    m_clips.clear();
}

void QuadBatch::add(Rect rect, std::array<f32, 4> uv, u32 color, u32 flags) {
    if (rect.width <= 0.0F || rect.height <= 0.0F) {
        return;
    }
    if (!m_clips.empty()) {
        const Rect clipped = intersect(rect, m_clips.back());
        if (clipped.width <= 0.0F || clipped.height <= 0.0F) {
            return;
        }
        if ((flags & kFlagTextured) != 0) {
            // Cut the texture coordinates by the same fractions as the rectangle.
            const f32 du = (uv[2] - uv[0]) / rect.width;
            const f32 dv = (uv[3] - uv[1]) / rect.height;
            uv = {uv[0] + (clipped.x - rect.x) * du, uv[1] + (clipped.y - rect.y) * dv,
                  uv[0] + (clipped.x + clipped.width - rect.x) * du,
                  uv[1] + (clipped.y + clipped.height - rect.y) * dv};
        }
        rect = clipped;
    }
    m_quads.push_back(
        {.rect = {rect.x, rect.y, rect.width, rect.height}, .uv = uv, .color = color, .flags = flags});
}

void QuadBatch::fillRect(const Rect& rect, u32 color) {
    if ((color >> 24U) == 0) {
        return;
    }
    add({.x = std::round(rect.x),
         .y = std::round(rect.y),
         .width = std::round(rect.width),
         .height = std::round(rect.height)},
        {}, color, 0);
}

bool QuadBatch::image(const Rect& rect, std::string_view name, u32 tint) {
    const AtlasRegion* region = m_resources != nullptr ? m_resources->image(name) : nullptr;
    if (region == nullptr) {
        return false;
    }
    add(rect, {region->u0, region->v0, region->u1, region->v1}, tint, kFlagTextured);
    return true;
}

bool QuadBatch::nineSlice(const Rect& rect, std::string_view name, f32 border, f32 scale, u32 tint) {
    const AtlasRegion* region = m_resources != nullptr ? m_resources->image(name) : nullptr;
    if (region == nullptr) {
        return false;
    }
    // Borders in texels, at most half the image; in window pixels, at most
    // half the target, so tiny targets shrink their corners instead of
    // overlapping them.
    const f32 bx = std::min(border, static_cast<f32>(region->width) / 2.0F);
    const f32 by = std::min(border, static_cast<f32>(region->height) / 2.0F);
    const f32 px = std::min(bx * scale, rect.width / 2.0F);
    const f32 py = std::min(by * scale, rect.height / 2.0F);
    const f32 fx = bx / static_cast<f32>(region->width);
    const f32 fy = by / static_cast<f32>(region->height);
    const std::array<f32, 4> xs = {rect.x, rect.x + px, rect.x + rect.width - px, rect.x + rect.width};
    const std::array<f32, 4> ys = {rect.y, rect.y + py, rect.y + rect.height - py, rect.y + rect.height};
    const std::array<f32, 4> us = {0.0F, fx, 1.0F - fx, 1.0F};
    const std::array<f32, 4> vs = {0.0F, fy, 1.0F - fy, 1.0F};
    for (usize row = 0; row < 3; ++row) {
        for (usize column = 0; column < 3; ++column) {
            add({.x = xs[column],
                 .y = ys[row],
                 .width = xs[column + 1] - xs[column],
                 .height = ys[row + 1] - ys[row]},
                subRegion(*region, us[column], vs[row], us[column + 1], vs[row + 1]), tint, kFlagTextured);
        }
    }
    return true;
}

void QuadBatch::addGlyphs(const PixelFont& font, std::string_view text, f32 x, f32 y, u32 scale, u32 color) {
    m_glyphs.clear();
    font.layout(text, m_glyphs);
    const auto size = static_cast<f32>(scale);
    const AtlasRegion& sheet = m_resources->fontRegion;
    for (const GlyphQuad& glyph : m_glyphs) {
        // Glyph coordinates are fractions of the font sheet; the sheet is
        // one region of the atlas.
        const Glyph& shape = *glyph.glyph;
        add({.x = x + glyph.x * size,
             .y = y + glyph.y * size,
             .width = glyph.width * size,
             .height = glyph.height * size},
            subRegion(sheet, shape.u0, shape.v0, shape.u1, shape.v1), color, kFlagTextured);
    }
}

void QuadBatch::text(std::string_view text, f32 x, f32 y, const TextStyle& style) {
    if (m_resources == nullptr || !m_resources->font || text.empty()) {
        return;
    }
    const u32 scale = std::max(style.scale, 1U);
    const f32 left = std::round(x);
    const f32 top = std::round(y);
    if (style.shadow) {
        addGlyphs(*m_resources->font, text, left + static_cast<f32>(scale), top + static_cast<f32>(scale),
                  scale, shadowOf(style.color));
    }
    addGlyphs(*m_resources->font, text, left, top, scale, style.color);
}

void QuadBatch::pushClip(const Rect& rect) {
    m_clips.push_back(m_clips.empty() ? rect : intersect(rect, m_clips.back()));
}

void QuadBatch::popClip() {
    if (!m_clips.empty()) {
        m_clips.pop_back();
    }
}

void QuadBatch::append(const QuadBatch& other) {
    m_quads.insert(m_quads.end(), other.m_quads.begin(), other.m_quads.end());
}

} // namespace vxe::ui
