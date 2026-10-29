#include "../test_helpers.hpp"

#include <vxe/io/file_io.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/pixel_font.hpp>
#include <vxe/ui/ui_renderer.hpp>

#include <doctest/doctest.h>
#include <stb_image.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace vxe;

namespace {

// A sheet with glyphs given by their width in texels: each one a filled
// block from its cell's left edge.
std::vector<u32> sheet(std::initializer_list<std::pair<char32_t, u32>> widths, u32 cell = 8) {
    const u32 size = cell * 16;
    std::vector<u32> pixels(usize{size} * size, 0U);
    for (const auto& [code, width] : widths) {
        const u32 x0 = (static_cast<u32>(code) % 16) * cell;
        const u32 y0 = (static_cast<u32>(code) / 16) * cell;
        for (u32 y = 0; y < cell; ++y) {
            for (u32 x = 0; x < width; ++x) {
                pixels[usize{y0 + y} * size + x0 + x] = 0xFFFFFFFFU;
            }
        }
    }
    return pixels;
}

ui::PixelFont makeFont(std::initializer_list<std::pair<char32_t, u32>> widths, u32 cell = 8) {
    const std::vector<u32> pixels = sheet(widths, cell);
    auto font = ui::PixelFont::fromSheet(pixels, cell * 16, cell * 16);
    REQUIRE(font.hasValue());
    return std::move(font).value();
}

} // namespace

TEST_CASE("UTF-8 decodes into code points, malformed bytes into U+FFFD") {
    const std::string text = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x99\x82\x80\xC3";
    std::vector<char32_t> decoded;
    for (usize position = 0; position < text.size();) {
        decoded.push_back(ui::nextCodePoint(text, position));
    }
    CHECK(decoded == std::vector<char32_t>{U'A', 0xE9, 0x20AC, 0x1F642, 0xFFFD, 0xFFFD});
}

TEST_CASE("Glyph widths come from the opaque pixels; space and missing glyphs have fixed rules") {
    const ui::PixelFont font = makeFont({{0, 5}, {U'i', 1}, {U'm', 5}, {U'A', 5}});
    CHECK(font.glyph(U'i').width == 1.0F);
    CHECK(font.glyph(U'i').advance == 2.0F);
    CHECK(font.glyph(U'm').advance == 6.0F);
    CHECK(font.hasGlyph(U' '));
    CHECK_FALSE(font.hasGlyph(U'q'));
    CHECK(&font.glyph(U'q') == &font.glyph(0)); // The box.
    CHECK(&font.glyph(0x20AC) == &font.glyph(0));
    // u runs from the cell's left edge to the glyph's last column.
    CHECK(font.glyph(U'A').u0 == doctest::Approx(1.0 / 16.0));
    CHECK(font.glyph(U'A').u1 == doctest::Approx((8.0 + 5.0) / 128.0));
    CHECK(font.glyph(U'A').v0 == doctest::Approx(4.0 / 16.0));
}

TEST_CASE("Text is measured and laid out in font pixels") {
    const ui::PixelFont font = makeFont({{U'i', 1}, {U'm', 5}});
    CHECK(font.measure("").width == 0.0F);
    CHECK(font.measure("mi").width == 7.0F);   // 5 + 1 + 1.
    CHECK(font.measure("m i").width == 11.0F); // 5 + 1, space 3 + 1, 1.
    CHECK(font.measure("m").height == ui::kFontCellPixels);
    const ui::TextSize twoLines = font.measure("mm\ni");
    CHECK(twoLines.width == 11.0F);
    CHECK(twoLines.height == ui::kFontLineHeight + ui::kFontCellPixels);

    std::vector<ui::GlyphQuad> quads;
    font.layout("m i\nm", quads);
    REQUIRE(quads.size() == 3); // Spaces draw nothing.
    CHECK(quads[0].x == 0.0F);
    CHECK(quads[1].x == 10.0F);
    CHECK(quads[1].width == 1.0F);
    CHECK(quads[2].x == 0.0F);
    CHECK(quads[2].y == ui::kFontLineHeight);
}

TEST_CASE("A higher resolution sheet keeps the size of text") {
    const ui::PixelFont low = makeFont({{U'm', 5}}, 8);
    const ui::PixelFont high = makeFont({{U'm', 10}}, 16);
    CHECK(high.texelsPerPixel() == 2);
    CHECK(high.glyph(U'm').width == low.glyph(U'm').width);
    CHECK(high.measure("mmm").width == low.measure("mmm").width);
}

TEST_CASE("Sheets must be square grids of 16 x 16 cells") {
    const std::vector<u32> wide(usize{256} * 128, 0U);
    CHECK_FALSE(ui::PixelFont::fromSheet(wide, 256, 128).hasValue());
    const std::vector<u32> odd(usize{100} * 100, 0U);
    CHECK_FALSE(ui::PixelFont::fromSheet(odd, 100, 100).hasValue());
}

TEST_CASE("Wrapping breaks at spaces and splits words longer than a line") {
    const ui::PixelFont font = makeFont({{U'a', 3}}); // Advance 4.
    // "aa" is 7 wide, "aa aa" 19 wide (7, a 4 pixel space, 1 spacing, 7).
    CHECK(font.wrap("aa aa aa", 19.0F) == "aa aa\naa");
    CHECK(font.wrap("aa aa aa", 18.0F) == "aa\naa\naa");
    CHECK(font.wrap("aa aa aa", 100.0F) == "aa aa aa");
    CHECK(font.wrap("aaaaa", 11.0F) == "aaa\naa");
    CHECK(font.wrap("aa\naa aa", 19.0F) == "aa\naa aa");
    for (const std::string_view text : {"aa aa aa aa", "a aaaa aa a aaa"}) {
        const std::string wrapped = font.wrap(text, 19.0F);
        usize start = 0;
        while (start <= wrapped.size()) {
            const usize end = std::min(wrapped.find('\n', start), wrapped.size());
            CHECK(font.measure(std::string_view(wrapped).substr(start, end - start)).width <= 19.0F);
            start = end + 1;
        }
    }
}

TEST_CASE("The default pack's font has every printable ASCII character") {
    const auto bytes = io::readFile(std::filesystem::path(VXE_SOURCE_DIR) / "textures/default/ui/font.png");
    REQUIRE(bytes.hasValue());
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.value().data()),
                              static_cast<int>(bytes.value().size()), &width, &height, &channels, 4);
    REQUIRE(data != nullptr);
    std::vector<u32> pixels(static_cast<usize>(width) * static_cast<usize>(height));
    std::memcpy(pixels.data(), data, pixels.size() * sizeof(u32));
    stbi_image_free(data);
    auto font = ui::PixelFont::fromSheet(pixels, static_cast<u32>(width), static_cast<u32>(height));
    REQUIRE(font.hasValue());
    for (char32_t code = 0x21; code < 0x7F; ++code) {
        CAPTURE(static_cast<u32>(code));
        CHECK(font.value().hasGlyph(code));
        CHECK(font.value().glyph(code).width <= ui::kFontCellPixels);
    }
    // Proportional: an i is narrower than an m.
    CHECK(font.value().glyph(U'i').width < font.value().glyph(U'm').width);
}

TEST_CASE("The UI renderer batches rectangles and text into one draw") {
    const auto device = rhi::createNullDevice();
    auto created = ui::UiRenderer::create({.device = device.get(),
                                           .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR),
                                           .targetFormat = rhi::Format::RGBA8Unorm});
    REQUIRE(created.hasValue());
    ui::UiRenderer& renderer = *created.value();
    rhi::CommandList commands;

    renderer.begin(1280, 720);
    renderer.drawText("no font yet", 10.0F, 10.0F);
    CHECK(renderer.quadCount() == 0);

    const std::vector<u32> pixels = sheet({{0, 5}, {U'H', 5}, {U'i', 1}});
    REQUIRE(renderer.setFont(pixels, 128, 128).hasValue());
    renderer.begin(1280, 720);
    renderer.fillRect({.x = 0.0F, .y = 0.0F, .width = 100.0F, .height = 20.0F}, ui::rgba(0, 0, 0, 128));
    renderer.drawText("Hi there", 10.4F, 20.6F, {.scale = 3, .shadow = true});
    // One rectangle, then 7 glyphs twice (the shadow first).
    REQUIRE(renderer.quadCount() == 15);
    CHECK(renderer.measureText("Hi", 3).width == 21.0F);
    CHECK(renderer.measureText("Hi", 3).height == 24.0F);
    for (usize frame = 0; frame < 5; ++frame) {
        renderer.record(commands);
        device->submit(commands);
        commands.reset();
    }
    // Replacing the font and destroying the renderer release everything.
    REQUIRE(renderer.setFont(pixels, 128, 128).hasValue());
    created.value().reset();
    device->waitIdle();
    CHECK(device->liveResources().empty());
}
