#include "vxe/ui/pixel_font.hpp"

#include <algorithm>
#include <format>

namespace vxe::ui {
namespace {

constexpr char32_t kReplacement = 0xFFFD;
constexpr char32_t kSpace = U' ';
constexpr f32 kSpaceWidth = 3.0F;
constexpr f32 kGlyphSpacing = 1.0F;

// A continuation byte of a UTF-8 sequence: 10xxxxxx.
constexpr bool isContinuation(u8 byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

} // namespace

char32_t nextCodePoint(std::string_view text, usize& position) noexcept {
    const auto lead = static_cast<u8>(text[position]);
    if (lead < 0x80U) {
        ++position;
        return lead;
    }
    usize length = 0;
    char32_t value = 0;
    if (lead >= 0xC2U && lead < 0xE0U) {
        length = 2;
        value = lead & 0x1FU;
    } else if (lead >= 0xE0U && lead < 0xF0U) {
        length = 3;
        value = lead & 0x0FU;
    } else if (lead >= 0xF0U && lead < 0xF5U) {
        length = 4;
        value = lead & 0x07U;
    } else {
        ++position; // A stray continuation byte or a lead byte UTF-8 never uses.
        return kReplacement;
    }
    if (position + length > text.size()) {
        ++position;
        return kReplacement;
    }
    for (usize i = 1; i < length; ++i) {
        const auto byte = static_cast<u8>(text[position + i]);
        if (!isContinuation(byte)) {
            ++position;
            return kReplacement;
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    position += length;
    return value;
}

Result<PixelFont> PixelFont::fromSheet(std::span<const u32> pixels, u32 width, u32 height) {
    if (width != height || width == 0 || width % 16 != 0 || pixels.size() != usize{width} * height) {
        return Error(
            ErrorCode::InvalidArgument,
            std::format("A font sheet is square, 16 x 16 cells; this one is {} x {}", width, height));
    }
    PixelFont font;
    const u32 cell = width / 16;
    // Cells that are not a multiple of 8 texels (a 12 texel cell) still work:
    // font pixels then cover fractional texels.
    const f32 texelsPerPixel = static_cast<f32>(cell) / kFontCellPixels;
    font.m_texelsPerPixel = std::max(1U, cell / 8);
    const f32 size = static_cast<f32>(width);
    for (u32 code = 0; code < 256; ++code) {
        const u32 cellX = (code % 16) * cell;
        const u32 cellY = (code / 16) * cell;
        // The glyph ends at its rightmost column with any visible pixel.
        u32 columns = 0;
        for (u32 y = 0; y < cell; ++y) {
            const usize row = usize{cellY + y} * width + cellX;
            for (u32 x = cell; x > columns; --x) {
                if ((pixels[row + x - 1] >> 24U) != 0) {
                    columns = x;
                    break;
                }
            }
        }
        Glyph& glyph = font.m_glyphs[code];
        glyph.width = static_cast<f32>(columns) / texelsPerPixel;
        glyph.advance = glyph.width + kGlyphSpacing;
        glyph.u0 = static_cast<f32>(cellX) / size;
        glyph.u1 = static_cast<f32>(cellX + columns) / size;
        glyph.v0 = static_cast<f32>(cellY) / size;
        glyph.v1 = static_cast<f32>(cellY + cell) / size;
        font.m_present[code] = columns > 0;
    }
    font.m_present[kSpace] = true;
    return font;
}

bool PixelFont::hasGlyph(char32_t codePoint) const noexcept {
    return codePoint < 256 && m_present[codePoint];
}

const Glyph& PixelFont::glyph(char32_t codePoint) const noexcept {
    return hasGlyph(codePoint) ? m_glyphs[codePoint] : m_glyphs[0];
}

f32 PixelFont::advanceOf(char32_t codePoint) const noexcept {
    return codePoint == kSpace ? kSpaceWidth + kGlyphSpacing : glyph(codePoint).advance;
}

TextSize PixelFont::measure(std::string_view text) const noexcept {
    if (text.empty()) {
        return {};
    }
    f32 width = 0.0F;
    f32 line = 0.0F;
    usize lines = 1;
    for (usize position = 0; position < text.size();) {
        const char32_t codePoint = nextCodePoint(text, position);
        if (codePoint == U'\n') {
            width = std::max(width, line);
            line = 0.0F;
            ++lines;
            continue;
        }
        line += advanceOf(codePoint);
    }
    width = std::max(width, line);
    // The spacing after the last glyph is not part of the text.
    return {.width = std::max(0.0F, width - kGlyphSpacing),
            .height = static_cast<f32>(lines - 1) * kFontLineHeight + kFontCellPixels};
}

void PixelFont::layout(std::string_view text, std::vector<GlyphQuad>& out) const {
    f32 x = 0.0F;
    f32 y = 0.0F;
    for (usize position = 0; position < text.size();) {
        const char32_t codePoint = nextCodePoint(text, position);
        if (codePoint == U'\n') {
            x = 0.0F;
            y += kFontLineHeight;
            continue;
        }
        if (codePoint != kSpace) {
            const Glyph& shape = glyph(codePoint);
            out.push_back({.x = x, .y = y, .width = shape.width, .height = kFontCellPixels, .glyph = &shape});
        }
        x += advanceOf(codePoint);
    }
}

std::string PixelFont::ellipsize(std::string_view text, f32 maxWidth) const {
    if (measure(text).width <= maxWidth) {
        return std::string(text);
    }
    constexpr std::string_view kDots = "...";
    // Widths add up glyph by glyph: the prefix width plus the dots' width.
    const f32 dots = measure(kDots).width + kGlyphSpacing;
    f32 width = 0.0F;
    usize fits = 0;
    for (usize position = 0; position < text.size();) {
        const usize begin = position;
        width += advanceOf(nextCodePoint(text, position));
        if (width + dots - kGlyphSpacing > maxWidth) {
            break;
        }
        fits = position;
        static_cast<void>(begin);
    }
    // No space before the dots: "Frame rate in the..." reads better than "the ...".
    while (fits > 0 && text[fits - 1] == ' ') {
        --fits;
    }
    return std::string(text.substr(0, fits)) + std::string(kDots);
}

std::string PixelFont::wrap(std::string_view text, f32 maxWidth) const {
    std::string result;
    result.reserve(text.size() + 8);
    // The current line in `result` starts at lineStart; lastSpace is the
    // last space in it (where it can be broken), npos for none.
    usize lineStart = 0;
    usize lastSpace = std::string::npos;
    f32 width = 0.0F;
    f32 widthAtSpace = 0.0F;
    for (usize position = 0; position < text.size();) {
        const usize begin = position;
        const char32_t codePoint = nextCodePoint(text, position);
        if (codePoint == U'\n') {
            result.push_back('\n');
            lineStart = result.size();
            lastSpace = std::string::npos;
            width = 0.0F;
            continue;
        }
        const f32 advance = advanceOf(codePoint);
        if (codePoint != kSpace && width + advance - kGlyphSpacing > maxWidth && result.size() > lineStart) {
            if (lastSpace != std::string::npos) {
                // Break at the last space: the rest of the line moves down.
                result[lastSpace] = '\n';
                width -= widthAtSpace;
                lineStart = lastSpace + 1;
            } else {
                result.push_back('\n'); // One word longer than the line.
                lineStart = result.size();
                width = 0.0F;
            }
            lastSpace = std::string::npos;
        }
        if (codePoint == kSpace) {
            if (result.size() == lineStart) {
                continue; // Lines do not start with the space they broke at.
            }
            lastSpace = result.size();
            widthAtSpace = width + advance;
        }
        result.append(text.substr(begin, position - begin));
        width += advance;
    }
    return result;
}

} // namespace vxe::ui
