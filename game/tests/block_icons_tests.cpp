#include "client/block_icons.hpp"
#include "common/block_catalog.hpp"

#include <vxe/voxel/block_registry.hpp>

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace vr;
using vxe::u32;

namespace {

vxe::render::PackImage solid(u32 color, u32 frames = 1) {
    vxe::render::PackImage image;
    image.width = 16;
    image.height = 16 * frames;
    image.pixels.assign(std::size_t{image.width} * image.height, color);
    return image;
}

u32 at(const BlockIcons::Icon& icon, u32 x, u32 y) {
    return icon.pixels[(std::size_t{y} * BlockIcons::kSize) + x];
}

} // namespace

TEST_CASE("Block icons show three shaded faces with tints and overlays") {
    const std::vector<BlockSource> sources = {
        {.file = "grass.toml",
         .text = "name = \"grass\"\ndisplay_name = \"Grass\"\nlayer = \"opaque\"\ntint = 1\n"
                 "tint_color = \"#00FF00\"\ntint_faces = [\"top\", \"side\"]\n"
                 "[textures]\ntop = \"top\"\nside = \"side\"\nbottom = \"bottom\"\n"
                 "[overlays]\nside = \"fringe\""},
    };
    const auto catalog = BlockCatalog::load(sources);
    REQUIRE(catalog.hasValue());

    const vxe::render::PackImage top = solid(0xFFFFFFFFU);        // White: only its tint shows.
    const vxe::render::PackImage side = solid(0xFF0000FFU);       // Red.
    const vxe::render::PackImage clear = solid(0x00000000U);      // An overlay that covers nothing.
    const vxe::render::PackImage bottom = solid(0xFF000000U, 32); // A strip, like an animation.
    BlockIcons icons;
    icons.bake(catalog.value(), [&](std::string_view key) -> const vxe::render::PackImage* {
        if (key == "top") {
            return &top;
        }
        if (key == "side") {
            return &side;
        }
        return key == "fringe" ? &clear : &bottom;
    });
    REQUIRE(icons.icons().size() == 1);
    const BlockIcons::Icon& icon = icons.icons().front();
    CHECK(icon.name == "icon/grass");
    CHECK(icons.iconOf(*catalog.value().stateId("grass")) == "icon/grass");
    CHECK(icons.iconOf(0).empty()); // Air has none.

    CHECK(at(icon, 16, 8) == 0xFF00FF00U); // The top: white times the tint.
    CHECK((at(icon, 0, 0) >> 24U) == 0);   // Outside the cube: clear.
    CHECK((at(icon, 31, 31) >> 24U) == 0);
    const u32 left = at(icon, 8, 22) & 0xFFU;   // Red, darkened.
    const u32 right = at(icon, 24, 22) & 0xFFU; // Red, darker still.
    CHECK(left > right);
    CHECK(left > 0);
}

TEST_CASE("A missing texture shows as magenta, not as a crash") {
    const std::vector<BlockSource> sources = {
        {.file = "a.toml",
         .text = "name = \"a\"\ndisplay_name = \"A\"\nlayer = \"opaque\"\n[textures]\nall = \"a\""},
    };
    const auto catalog = BlockCatalog::load(sources);
    REQUIRE(catalog.hasValue());
    BlockIcons icons;
    icons.bake(catalog.value(), [](std::string_view) { return nullptr; });
    REQUIRE(icons.icons().size() == 1);
    CHECK(at(icons.icons().front(), 16, 8) == 0xFFFF00FFU);
}
