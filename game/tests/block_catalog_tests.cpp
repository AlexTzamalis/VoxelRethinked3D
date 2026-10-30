#include "common/block_catalog.hpp"

#include <vxe/voxel/block_registry.hpp>

#include <doctest/doctest.h>

#include <string>
#include <vector>

using vr::BlockCatalog;
using vr::BlockSource;
using vxe::voxel::BlockFace;
using vxe::voxel::RenderLayer;

namespace {

std::string errorOf(std::vector<BlockSource> sources) {
    auto catalog = BlockCatalog::load(sources);
    REQUIRE_FALSE(catalog.hasValue());
    return catalog.error().message();
}

vxe::u16 layerOf(const BlockCatalog& catalog, std::string_view state, BlockFace face) {
    const auto id = catalog.stateId(state);
    REQUIRE(id.has_value());
    return catalog.renderInfo(*id).textureLayer(face);
}

std::string_view textureOf(const BlockCatalog& catalog, std::string_view state, BlockFace face) {
    return catalog.textureKeys()[layerOf(catalog, state, face)];
}

} // namespace

TEST_CASE("Every shipped block definition loads") {
    auto loaded = BlockCatalog::loadFolder(VR_BLOCKS_DIR);
    REQUIRE_MESSAGE(loaded.hasValue(), (loaded ? std::string() : loaded.error().message()));
    const BlockCatalog& catalog = loaded.value();
    MESSAGE(catalog.blockCount() << " blocks, " << catalog.stateCount() << " states, "
                                 << catalog.textureKeys().size() << " texture keys");

    // The G1.1 list.
    for (const std::string_view name :
         {"stone", "dirt", "grass_block", "sand", "gravel", "water", "oak_log", "oak_leaves", "glass",
          "lumen_block", "snow_block", "coal_ore", "iron_ore", "gold_ore", "foundation_stone"}) {
        INFO(name);
        CHECK(catalog.block(name) != nullptr);
    }
    CHECK(catalog.stateName(0) == "air");
    CHECK(catalog.renderInfo(0).layer == RenderLayer::Invisible);

    // Render properties come through.
    CHECK(catalog.renderInfo(*catalog.defaultState("water")).layer == RenderLayer::Translucent);
    CHECK(catalog.renderInfo(*catalog.defaultState("oak_leaves")).layer == RenderLayer::Cutout);
    CHECK_FALSE(catalog.renderInfo(*catalog.defaultState("oak_leaves")).cullsSameType);
    CHECK(catalog.renderInfo(*catalog.defaultState("lumen_block")).emittedLight == 15);
    CHECK(catalog.renderInfo(*catalog.defaultState("stone")).lightOpacity == 15);
    CHECK(catalog.renderInfo(*catalog.defaultState("glass")).lightOpacity == 0);

    // Faces: grass has its own top, dirt below and a grassy side.
    CHECK(textureOf(catalog, "grass_block", BlockFace::PosY) == "grass_top");
    CHECK(textureOf(catalog, "grass_block", BlockFace::NegY) == "dirt");
    CHECK(textureOf(catalog, "grass_block", BlockFace::PosX) == "grass_side");
    // Logs: three states, the rings on the ends of their axis.
    CHECK(catalog.block("oak_log")->stateCount == 3);
    CHECK(textureOf(catalog, "oak_log[axis=y]", BlockFace::PosY) == "oak_log_top");
    CHECK(textureOf(catalog, "oak_log[axis=y]", BlockFace::PosX) == "oak_log_side");
    CHECK(textureOf(catalog, "oak_log[axis=x]", BlockFace::NegX) == "oak_log_top");
    CHECK(textureOf(catalog, "oak_log[axis=z]", BlockFace::PosY) == "oak_log_side");

    // The engine's table matches the catalog state for state.
    auto view = catalog.buildRegistry();
    REQUIRE(view.hasValue());
    REQUIRE(view.value()->size() == catalog.stateCount());
    for (vxe::usize state = 0; state < catalog.stateCount(); ++state) {
        CHECK(view.value()->info(static_cast<vxe::voxel::BlockStateId>(state)) ==
              catalog.renderInfo(static_cast<vxe::voxel::BlockStateId>(state)));
    }
}

TEST_CASE("State ids follow file names and names map both ways") {
    const std::vector<BlockSource> sources = {
        {.file = "b.toml",
         .text = "name = \"bee\"\ndisplay_name = \"B\"\nlayer = \"opaque\"\n[textures]\nall = \"b\""},
        {.file = "a.toml",
         .text = "name = \"ant\"\ndisplay_name = \"A\"\nlayer = \"opaque\"\n[textures]\nall = \"a\""},
    };
    auto catalog = BlockCatalog::load(sources);
    REQUIRE(catalog.hasValue());
    CHECK(catalog.value().stateId("ant") ==
          vxe::voxel::BlockStateId{1}); // a.toml first, whatever the order given.
    CHECK(catalog.value().stateId("bee") == vxe::voxel::BlockStateId{2});
    CHECK(catalog.value().stateName(2) == "bee");
    CHECK_FALSE(catalog.value().stateId("cat").has_value());
    CHECK(catalog.value().textureKeys().size() == 2);
}

TEST_CASE("Tints and overlays color the faces their block names") {
    const std::vector<BlockSource> sources = {
        {.file = "grass.toml",
         .text = "name = \"grass\"\ndisplay_name = \"Grass\"\nlayer = \"opaque\"\ntint = 1\n"
                 "tint_color = \"#804020\"\ntint_faces = [\"top\", \"side\"]\n"
                 "[textures]\ntop = \"g_top\"\nside = \"g_side\"\nbottom = \"dirt\"\n"
                 "[overlays]\nside = \"g_fringe\""},
    };
    const auto catalog = BlockCatalog::load(sources);
    REQUIRE(catalog.hasValue());
    const auto keys = catalog.value().textureKeys();
    const auto layer = [&](std::string_view key) {
        return static_cast<vxe::usize>(std::ranges::find(keys, key) - keys.begin());
    };
    const auto looks = catalog.value().textureLooks();
    REQUIRE(looks.size() == keys.size());
    CHECK(looks[layer("g_top")] == vxe::voxel::textureLook(0, 1, 0));
    CHECK(looks[layer("dirt")] == 0); // The underside stays plain.
    CHECK(looks[layer("g_side")] ==
          vxe::voxel::textureLook(static_cast<vxe::u32>(layer("g_fringe")) + 1, 0, 1)); // Only the fringe.
    CHECK(catalog.value().tintColors()[1] == 0xFF204080U);

    // Far away, a tinted texture shows its tint, and half an overlay shows half of it.
    std::vector<vxe::u32> average(keys.size(), 0xFFFFFFFFU);
    average[layer("g_fringe")] = 0x80FFFFFFU;
    average[layer("g_side")] = 0xFF000000U;
    const std::vector<vxe::u32> shown = catalog.value().shownColors(average);
    CHECK(shown[layer("g_top")] == 0xFF204080U);
    CHECK(shown[layer("dirt")] == 0xFFFFFFFFU);
    CHECK((shown[layer("g_side")] & 0xFFU) == 0x40U);
}

TEST_CASE("Invalid block files report every problem with file and field") {
    // Syntax errors name the line.
    CHECK(errorOf({{.file = "broken.toml", .text = "name = \"x\nlayer = "}})
              .find("broken.toml: not valid TOML (line") != std::string::npos);

    // Several problems in several files, all reported at once.
    const std::string message = errorOf({
        {.file = "one.toml",
         .text = "name = \"Bad Name\"\ndisplay_name = \"X\"\nlayer = \"opaque\"\n[textures]\nall = \"x\""},
        {.file = "two.toml",
         .text = "name = \"two\"\ndisplay_name = \"Two\"\nlayer = \"solid\"\n[textures]\nall = \"x\""},
        {.file = "three.toml",
         .text = "name = \"three\"\ndisplay_name = \"Three\"\nlayer = \"opaque\"\nemitted_light = 20\n"
                 "[textures]\nall = \"x\""},
        {.file = "four.toml",
         .text = "name = \"four\"\ndisplay_name = \"Four\"\nlayer = \"opaque\"\n"
                 "[textures]\nside = \"x\"\ntop = \"y\""},
        {.file = "five.toml",
         .text = "name = \"five\"\ndisplay_name = \"Five\"\nlayer = \"opaque\"\ncolour = 3\n"
                 "[textures]\nall = \"x\"\nfront = \"y\""},
    });
    MESSAGE(message);
    CHECK(message.find("one.toml: 'name' must be lowercase") != std::string::npos);
    CHECK(message.find("two.toml: 'layer' must be opaque, cutout, translucent or invisible (got 'solid')") !=
          std::string::npos);
    CHECK(message.find("three.toml: 'emitted_light' must be a whole number from 0 to 15") !=
          std::string::npos);
    CHECK(message.find("four.toml: state 'four' has no texture for its down face") != std::string::npos);
    CHECK(message.find("five.toml: unknown field 'colour'") != std::string::npos);
    CHECK(message.find("five.toml: textures: unknown face 'front'") != std::string::npos);

    const std::string tints = errorOf({
        {.file = "a.toml",
         .text = "name = \"a\"\ndisplay_name = \"A\"\nlayer = \"opaque\"\ntint = 1\n"
                 "tint_color = \"green\"\n[textures]\nall = \"x\""},
        {.file = "b.toml",
         .text = "name = \"b\"\ndisplay_name = \"B\"\nlayer = \"opaque\"\ntint = 1\n"
                 "tint_faces = [\"front\"]\n[textures]\nall = \"x\""},
        {.file = "c.toml",
         .text = "name = \"c\"\ndisplay_name = \"C\"\nlayer = \"opaque\"\ntint = 1\n"
                 "[textures]\nall = \"shared\""},
        {.file = "d.toml",
         .text = "name = \"d\"\ndisplay_name = \"D\"\nlayer = \"opaque\"\n[textures]\nall = \"shared\""},
    });
    MESSAGE(tints);
    CHECK(tints.find("a.toml: 'tint_color' must look like") != std::string::npos);
    CHECK(tints.find("b.toml: tint_faces: unknown face 'front'") != std::string::npos);
    CHECK(tints.find("d.toml: texture 'shared' is tinted or overlaid in two different ways") !=
          std::string::npos);

    // Duplicates and empty folders.
    const BlockSource stone{
        .file = "stone.toml",
        .text = "name = \"stone\"\ndisplay_name = \"S\"\nlayer = \"opaque\"\n[textures]\nall = \"s\""};
    BlockSource copy = stone;
    copy.file = "stone_copy.toml";
    CHECK(errorOf({stone, copy}).find("stone_copy.toml: block 'stone' is defined twice") !=
          std::string::npos);
    CHECK(errorOf({}).find("no block definitions found") != std::string::npos);
    CHECK_FALSE(BlockCatalog::loadFolder("this/folder/does/not/exist").hasValue());
}
