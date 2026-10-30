#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/ui/quad_batch.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widgets.hpp>

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace vxe;
using namespace vxe::ui;

namespace {

std::vector<u32> filled(u32 width, u32 height, u32 color) {
    return std::vector<u32>(usize{width} * height, color);
}

bool overlaps(const AtlasRegion& a, const AtlasRegion& b) {
    return a.u0 < b.u1 && b.u0 < a.u1 && a.v0 < b.v1 && b.v0 < a.v1;
}

// Resources with one 16 x 16 image "frame" filling the whole 0..1 square, so
// texture coordinates equal fractions of the image.
UiResources squareImage() {
    UiResources resources;
    resources.images.push_back(
        {"frame", {.u0 = 0.0F, .v0 = 0.0F, .u1 = 1.0F, .v1 = 1.0F, .width = 16, .height = 16}});
    return resources;
}

} // namespace

TEST_CASE("The atlas packs images without overlap and copies their pixels") {
    const std::vector<u32> font = filled(128, 128, 0xFF0000FFU);
    const std::vector<u32> wide = filled(200, 20, 0xFF00FF00U);
    const std::vector<u32> small = filled(16, 16, 0xFFFF0000U);
    const std::vector<AtlasImage> images = {{.name = "font", .width = 128, .height = 128, .pixels = font},
                                            {.name = "wide", .width = 200, .height = 20, .pixels = wide},
                                            {.name = "a", .width = 16, .height = 16, .pixels = small},
                                            {.name = "b", .width = 16, .height = 16, .pixels = small}};
    const AtlasPixels atlas = packAtlas(images);
    REQUIRE(atlas.regions.size() == 4);
    CHECK(atlas.width == 256);
    CHECK(atlas.height <= atlas.width);
    CHECK(atlas.pixels.size() == usize{atlas.width} * atlas.height);
    for (usize i = 0; i < atlas.regions.size(); ++i) {
        CHECK(atlas.regions[i].first == images[i].name); // Regions keep the input order.
        for (usize j = i + 1; j < atlas.regions.size(); ++j) {
            CHECK_FALSE(overlaps(atlas.regions[i].second, atlas.regions[j].second));
        }
    }
    // The pixels at each region's top left corner are its image's.
    for (usize i = 0; i < images.size(); ++i) {
        const AtlasRegion& region = atlas.regions[i].second;
        CHECK(region.width == images[i].width);
        const auto x = static_cast<usize>(region.u0 * static_cast<f32>(atlas.width));
        const auto y = static_cast<usize>(region.v0 * static_cast<f32>(atlas.height));
        CHECK(atlas.pixels[y * atlas.width + x] == images[i].pixels[0]);
    }
}

TEST_CASE("Clipping cuts rectangles and their texture coordinates") {
    const UiResources resources = squareImage();
    QuadBatch batch;
    batch.reset(&resources);
    batch.pushClip({.x = 10.0F, .y = 10.0F, .width = 100.0F, .height = 100.0F});
    batch.fillRect({.x = 0.0F, .y = 0.0F, .width = 50.0F, .height = 50.0F}, rgba(255, 0, 0));
    batch.fillRect({.x = 200.0F, .y = 0.0F, .width = 50.0F, .height = 50.0F}, rgba(255, 0, 0)); // Outside.
    REQUIRE(batch.image({.x = 0.0F, .y = 10.0F, .width = 40.0F, .height = 40.0F}, "frame"));
    batch.pushClip({.x = 0.0F, .y = 0.0F, .width = 30.0F, .height = 30.0F}); // Nested: 10..30.
    batch.fillRect({.x = 0.0F, .y = 0.0F, .width = 100.0F, .height = 100.0F}, rgba(0, 255, 0));
    batch.popClip();
    batch.popClip();
    batch.fillRect({.x = 0.0F, .y = 0.0F, .width = 5.0F, .height = 5.0F}, rgba(0, 0, 255)); // No clip left.

    const auto quads = batch.quads();
    REQUIRE(quads.size() == 4);
    CHECK(quads[0].rect == std::array<f32, 4>{10.0F, 10.0F, 40.0F, 40.0F});
    // The image lost its left quarter, and its u its first quarter.
    CHECK(quads[1].rect == std::array<f32, 4>{10.0F, 10.0F, 30.0F, 40.0F});
    CHECK(quads[1].uv[0] == doctest::Approx(0.25));
    CHECK(quads[1].uv[2] == doctest::Approx(1.0));
    CHECK(quads[1].uv[1] == doctest::Approx(0.0));
    CHECK(quads[2].rect == std::array<f32, 4>{10.0F, 10.0F, 20.0F, 20.0F});
    CHECK(quads[3].rect == std::array<f32, 4>{0.0F, 0.0F, 5.0F, 5.0F});
}

TEST_CASE("Nine slice frames keep their corners and stretch the rest") {
    const UiResources resources = squareImage();
    QuadBatch batch;
    batch.reset(&resources);
    CHECK_FALSE(
        batch.nineSlice({.x = 0.0F, .y = 0.0F, .width = 10.0F, .height = 10.0F}, "missing", 3.0F, 2.0F));
    REQUIRE(
        batch.nineSlice({.x = 100.0F, .y = 50.0F, .width = 200.0F, .height = 40.0F}, "frame", 4.0F, 2.0F));
    const auto quads = batch.quads();
    REQUIRE(quads.size() == 9);
    // Corners: 4 texels at scale 2 is 8 window pixels.
    CHECK(quads[0].rect == std::array<f32, 4>{100.0F, 50.0F, 8.0F, 8.0F});
    CHECK(quads[0].uv[2] == doctest::Approx(4.0 / 16.0));
    CHECK(quads[8].rect == std::array<f32, 4>{292.0F, 82.0F, 8.0F, 8.0F});
    CHECK(quads[8].uv[0] == doctest::Approx(12.0 / 16.0));
    // The center stretches over everything between the corners.
    CHECK(quads[4].rect == std::array<f32, 4>{108.0F, 58.0F, 184.0F, 24.0F});

    // A target smaller than two corners shrinks the corners instead of overlapping them.
    batch.reset(&resources);
    REQUIRE(batch.nineSlice({.x = 0.0F, .y = 0.0F, .width = 10.0F, .height = 10.0F}, "frame", 4.0F, 2.0F));
    CHECK(batch.quads()[0].rect == std::array<f32, 4>{0.0F, 0.0F, 5.0F, 5.0F});
}

TEST_CASE("Themes read nine slice corner sizes") {
    std::vector<std::string> warnings;
    const auto theme = Theme::parse("[slices]\npanel = 5\nbutton = 2\nicon = 1\n", warnings);
    REQUIRE(theme.hasValue());
    CHECK(theme.value().slices.panel == 5.0F);
    CHECK(theme.value().slices.button == 2.0F);
    CHECK(warnings.size() == 1);
}

TEST_CASE("The UI keeps its quads until something changes") {
    const auto device = rhi::createNullDevice();
    auto renderer = UiRenderer::create(
        {.device = device.get(), .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    REQUIRE(renderer.hasValue());
    UiRenderer& ui = *renderer.value();
    const std::vector<u32> font = filled(128, 128, 0xFFFFFFFFU);
    const std::vector<u32> image = filled(16, 16, 0xFF808080U);
    const std::vector<AtlasImage> images = {{.name = "button", .width = 16, .height = 16, .pixels = image}};
    REQUIRE(ui.setResources({.width = 128, .height = 128, .pixels = font}, images).hasValue());
    CHECK(ui.resources().image("button") != nullptr);
    CHECK(ui.resources().image("panel") == nullptr);

    config::SettingsRegistry settings;
    auto context = UiContext::create({.settings = &settings});
    auto& button = context->root().add<Button>("Hi");
    button.style().alignSelf = Align::Start;
    const auto frame = [&](f32 x, f32 y) {
        context->update(640, 480, {.x = x, .y = y}, ui);
        ui.begin(640, 480);
        context->draw(ui);
    };
    frame(-1.0F, -1.0F);
    const usize quads = ui.quadCount();
    CHECK(quads == 9 + 4); // The button image as nine slices, then "Hi" and its shadow.
    CHECK(context->rebuildCount() == 1);
    frame(-1.0F, -1.0F);
    frame(-1.0F, -1.0F);
    CHECK(context->rebuildCount() == 1); // Nothing changed: the kept quads were appended.
    CHECK(ui.quadCount() == quads);

    // Hovering changes the look: one rebuild, then kept again.
    frame(4.0F, 4.0F);
    frame(4.0F, 4.0F);
    CHECK(context->rebuildCount() == 2);
    CHECK(button.hovered());

    // New text changes the layout; new resources change the quads.
    button.setText("Hello");
    frame(4.0F, 4.0F);
    CHECK(context->rebuildCount() == 3);
    REQUIRE(ui.setFont(font, 128, 128).hasValue()); // Drops the button image: flat look.
    frame(4.0F, 4.0F);
    CHECK(context->rebuildCount() == 4);
    CHECK(ui.quadCount() != quads);

    context.reset();
    renderer.value().reset();
    device->waitIdle();
    CHECK(device->liveResources().empty());
}
