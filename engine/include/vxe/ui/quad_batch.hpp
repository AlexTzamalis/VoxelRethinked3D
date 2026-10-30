#pragma once

#include <vxe/core/types.hpp>
#include <vxe/ui/color.hpp>
#include <vxe/ui/pixel_font.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::ui {

// A rectangle in window pixels, origin at the top left, y down.
struct Rect {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;
};

struct TextStyle {
    // Window pixels per font pixel. Whole numbers keep pixel art sharp: each
    // font pixel covers the same number of window pixels.
    u32 scale = 2;
    u32 color = rgba(255, 255, 255);
    // A copy one font pixel down and right, the color darkened to a quarter,
    // which keeps text readable on any background.
    bool shadow = true;
};

// Where an image sits in the UI atlas: texture coordinates and its size in
// texels.
struct AtlasRegion {
    f32 u0 = 0.0F;
    f32 v0 = 0.0F;
    f32 u1 = 0.0F;
    f32 v1 = 0.0F;
    u32 width = 0;
    u32 height = 0;
};

// What batches draw with: the font and the images of the UI atlas (the
// font sheet and the packs' ui/*.png in one texture, so the whole UI is one
// draw call). UiRenderer owns it; `version` goes up with every change.
struct UiResources {
    std::optional<PixelFont> font;
    AtlasRegion fontRegion;
    std::vector<std::pair<std::string, AtlasRegion>> images;
    u64 version = 0;

    [[nodiscard]] const AtlasRegion* image(std::string_view name) const noexcept;
};

// One quad as the shader reads it (ui/quad.vert).
struct UiQuad {
    std::array<f32, 4> rect{}; // x, y, width, height in window pixels.
    std::array<f32, 4> uv{};   // u0, v0, u1, v1.
    u32 color = 0;
    u32 flags = 0; // Bit 0: sample the atlas.
    std::array<u32, 2> padding{};
};
static_assert(sizeof(UiQuad) == 48);

// Collects the quads of some UI in window pixels: rectangles, images,
// nine slice frames and text. Everything is clipped on the CPU against the
// current clip rectangle (texture coordinates cut to match), so scrolling
// lists need no scissor changes and the UI stays one draw call. A batch can
// be kept and appended again in later frames while nothing changed, which
// is how UiContext skips rebuilding a still screen.
//
// Thread safety: main thread only, like the resources it reads.
class QuadBatch {
public:
    // Clears the quads and the clip stack and draws with `resources` from now on.
    void reset(const UiResources* resources) noexcept;
    [[nodiscard]] const UiResources* resources() const noexcept { return m_resources; }

    void fillRect(const Rect& rect, u32 color);
    // A whole atlas image stretched over `rect`, tinted. False when the atlas
    // has no image of that name (the caller draws a fallback).
    bool image(const Rect& rect, std::string_view name, u32 tint = rgba(255, 255, 255));
    // An image as a nine slice frame: corners `border` texels wide keep their
    // size, edges stretch along, the center stretches both ways. `scale` is
    // window pixels per texel (the UI scale). False without the image.
    bool nineSlice(const Rect& rect, std::string_view name, f32 border, f32 scale,
                   u32 tint = rgba(255, 255, 255));
    // Text with its top left corner at (x, y), snapped to whole pixels.
    void text(std::string_view text, f32 x, f32 y, const TextStyle& style = {});

    // Everything drawn until the matching pop stays inside `rect` (and inside
    // the clips around it).
    void pushClip(const Rect& rect);
    void popClip();

    void append(const QuadBatch& other);
    [[nodiscard]] std::span<const UiQuad> quads() const noexcept { return m_quads; }
    [[nodiscard]] usize size() const noexcept { return m_quads.size(); }

private:
    // Adds a quad cut to the clip; uv is scaled along with the cut.
    void add(Rect rect, std::array<f32, 4> uv, u32 color, u32 flags);
    void addGlyphs(const PixelFont& font, std::string_view text, f32 x, f32 y, u32 scale, u32 color);

    const UiResources* m_resources = nullptr;
    std::vector<UiQuad> m_quads;
    std::vector<Rect> m_clips;
    std::vector<GlyphQuad> m_glyphs; // Reused layout scratch.
};

} // namespace vxe::ui
