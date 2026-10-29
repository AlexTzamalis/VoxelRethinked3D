#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::ui {

// Font pixels per glyph cell. Text is measured and laid out in font pixels,
// whatever the sheet's resolution: a 128 x 128 sheet has one texel per font
// pixel, a 256 x 256 sheet (from a 32 pixel texture pack) has 2 x 2, so a
// higher resolution font adds detail without changing the size of text.
inline constexpr f32 kFontCellPixels = 8.0F;
// Font pixels between the tops of two lines: a cell plus room for the drop
// shadow and a gap.
inline constexpr f32 kFontLineHeight = 10.0F;

// One character of the font, in font pixels and texture coordinates.
struct Glyph {
    // The drawn part of the cell: from the cell's left edge to the glyph's
    // last opaque column, the full cell high. v counts from the sheet's top
    // row (the UI renderer uploads the sheet top row first).
    f32 u0 = 0.0F;
    f32 v0 = 0.0F;
    f32 u1 = 0.0F;
    f32 v1 = 0.0F;
    f32 width = 0.0F;   // Drawn width.
    f32 advance = 0.0F; // How far the next character starts: width plus one pixel of spacing.
};

// One glyph placed by PixelFont::layout, in font pixels from the text's top
// left corner.
struct GlyphQuad {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;
    const Glyph* glyph = nullptr;
};

struct TextSize {
    f32 width = 0.0F;
    f32 height = 0.0F;
};

// Decodes the next code point of UTF-8 text at `position` and moves past it.
// Malformed bytes decode as U+FFFD, one byte at a time, so any text renders.
[[nodiscard]] char32_t nextCodePoint(std::string_view text, usize& position) noexcept;

// A bitmap font from a PNG sheet (E12.1): 16 x 16 square cells, one per
// code point 0 to 255 (row * 16 + column), glyphs left aligned in their
// cells and white on transparent, so text can take any color. The width of
// each glyph is measured from its opaque pixels, which makes the font
// proportional ("i" is narrower than "m") while the sheet stays a simple
// grid anyone can edit in an image editor. The space is 3 font pixels wide;
// empty cells and code points above 255 draw the glyph of code point 0 (a
// box) so missing characters stay visible.
//
// The engine's default sheet is textures/default/ui/font.png, an original
// font; packs replace it with their own ui/font.png (Section 15.1).
//
// Thread safety: immutable after creation; any thread may read it.
class PixelFont {
public:
    // Builds the font from an RGBA8 sheet (red in the lowest byte, rows from
    // the top). Fails with InvalidArgument unless the sheet is square and its
    // size a multiple of 16.
    [[nodiscard]] static Result<PixelFont> fromSheet(std::span<const u32> pixels, u32 width, u32 height);

    // The glyph of a code point; missing ones give the box of code point 0.
    [[nodiscard]] const Glyph& glyph(char32_t codePoint) const noexcept;
    // True when the sheet has a glyph for the code point (or it is a space).
    [[nodiscard]] bool hasGlyph(char32_t codePoint) const noexcept;
    // Sheet texels per font pixel (1 for a 128 x 128 sheet).
    [[nodiscard]] u32 texelsPerPixel() const noexcept { return m_texelsPerPixel; }

    // The size of text in font pixels. '\n' starts a new line; the height is
    // lines times kFontLineHeight, minus the gap below the last line.
    [[nodiscard]] TextSize measure(std::string_view text) const noexcept;
    // Places every visible glyph of `text`; appends to `out`.
    void layout(std::string_view text, std::vector<GlyphQuad>& out) const;
    // Breaks text into lines no wider than `maxWidth` font pixels, at spaces
    // where possible (a word longer than a line is split). Existing '\n's are
    // kept.
    [[nodiscard]] std::string wrap(std::string_view text, f32 maxWidth) const;

private:
    PixelFont() = default;
    [[nodiscard]] f32 advanceOf(char32_t codePoint) const noexcept;

    std::array<Glyph, 256> m_glyphs{};
    std::array<bool, 256> m_present{};
    u32 m_texelsPerPixel = 1;
};

} // namespace vxe::ui
