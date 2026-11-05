#include "common/block_catalog.hpp"

#include <vxe/voxel/block_registry.hpp>

#include <doctest/doctest.h>

#include <format>
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

TEST_CASE("Plants keep their variations in a row and tall plants know their halves") {
    const std::vector<BlockSource> sources = {
        {.file = "grass.toml",
         .text = "name = \"grass\"\ndisplay_name = \"G\"\nlayer = \"plant\"\ntint = 1\n"
                 "variations = [\"foliage/a\", \"foliage/b\", \"foliage/c\"]\n"
                 "[variation_overlays]\n\"foliage/b\" = \"foliage/seeds\""},
        {.file = "tall.toml",
         .text = "name = \"tall\"\ndisplay_name = \"T\"\nlayer = \"plant\"\ntall_top = \"tall_top\"\n"
                 "variations = [\"foliage/low\"]"},
        {.file = "tall_top.toml",
         .text = "name = \"tall_top\"\ndisplay_name = \"T\"\nlayer = \"plant\"\nhidden = true\n"
                 "variations = [\"foliage/high\"]"},
    };
    const auto loaded = BlockCatalog::load(sources);
    INFO((loaded.hasValue() ? std::string() : loaded.error().message()));
    REQUIRE(loaded.hasValue());
    const BlockCatalog& catalog = loaded.value();
    const auto grass = *catalog.stateId("grass");
    const auto& info = catalog.renderInfo(grass);
    CHECK(info.layer == vxe::voxel::RenderLayer::Plant);
    CHECK(info.variationCount == 3);
    const auto keys = catalog.textureKeys();
    const vxe::u16 first = info.textureLayer(BlockFace::PosX);
    CHECK(keys[first] == "foliage/a");
    CHECK(keys[first + 2U] == "foliage/c");
    // The plant is tinted; its seed heads keep their colors.
    const auto seeds = static_cast<vxe::u32>(std::ranges::find(keys, "foliage/seeds") - keys.begin());
    CHECK(catalog.textureLooks()[first + 1U] == vxe::voxel::textureLook(seeds + 1, 1, 0));
    CHECK(catalog.textureLooks()[first] == vxe::voxel::textureLook(0, 1, 0));

    CHECK_FALSE(catalog.isSolid(grass));
    CHECK(catalog.isReplaceable(grass));
    CHECK(catalog.needsSupport(grass));
    const auto tall = *catalog.stateId("tall");
    const auto top = *catalog.stateId("tall_top");
    CHECK(catalog.tallUpper(tall) == top);
    CHECK(catalog.tallLower(top) == tall);
    CHECK(catalog.tallUpper(grass) == vxe::voxel::kAirState);
    CHECK(catalog.block("tall_top")->hidden);
}

TEST_CASE("Solid and liquid come from the block files") {
    const std::vector<BlockSource> sources = {
        {.file = "stone.toml",
         .text = "name = \"stone\"\ndisplay_name = \"S\"\nlayer = \"opaque\"\n[textures]\nall = \"s\""},
        {.file = "water.toml",
         .text = "name = \"water\"\ndisplay_name = \"W\"\nlayer = \"translucent\"\nsolid = false\n"
                 "liquid = true\n[textures]\nall = \"w\""},
        {.file = "glass.toml",
         .text = "name = \"glass\"\ndisplay_name = \"G\"\nlayer = \"translucent\"\n[textures]\nall = \"g\""},
    };
    const auto catalog = BlockCatalog::load(sources);
    REQUIRE(catalog.hasValue());
    CHECK_FALSE(catalog.value().isSolid(0)); // Air.
    CHECK(catalog.value().isSolid(*catalog.value().stateId("stone")));
    CHECK(catalog.value().isSolid(*catalog.value().stateId("glass")));
    CHECK_FALSE(catalog.value().isSolid(*catalog.value().stateId("water")));
    CHECK(catalog.value().isLiquid(*catalog.value().stateId("water")));
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
    CHECK(message.find(
              "two.toml: 'layer' must be opaque, cutout, translucent, plant or invisible (got 'solid')") !=
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

    const std::string shared = errorOf({
        {.file = "a.toml",
         .text = "name = \"a\"\ndisplay_name = \"A\"\nlayer = \"plant\"\nvariations = [\"foliage/x\"]"},
        {.file = "b.toml",
         .text = "name = \"b\"\ndisplay_name = \"B\"\nlayer = \"plant\"\nvariations = [\"foliage/x\"]"},
    });
    CHECK(shared.find("b.toml: variation 'foliage/x' must be a new texture key") != std::string::npos);

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

TEST_CASE("Snow comes in layers that stack to a whole block") {
    std::string text = "name = \"snow\"\ndisplay_name = \"Snow\"\nlayer = \"opaque\"\nreplaceable = true\n"
                       "needs_support = true\n"
                       "[textures]\nall = \"s\"\n";
    for (int layers = 1; layers <= 8; ++layers) {
        text += std::format("[[variants]]\nproperties = {{ layers = \"{}\" }}\nheight = {}\n", layers,
                            layers * 2);
    }
    const std::vector<BlockSource> sources = {{.file = "snow.toml", .text = text}};
    const auto loaded = BlockCatalog::load(sources);
    INFO((loaded.hasValue() ? std::string() : loaded.error().message()));
    REQUIRE(loaded.hasValue());
    const BlockCatalog& catalog = loaded.value();
    const auto first = catalog.stateId("snow[layers=1]");
    const auto last = catalog.stateId("snow[layers=8]");
    REQUIRE(first.has_value());
    REQUIRE(last.has_value());
    CHECK(catalog.shapeHeight(*first) == doctest::Approx(2.0 / 16.0));
    CHECK(catalog.shapeHeight(*last) == doctest::Approx(1.0));
    CHECK(catalog.renderInfo(*last).height == 0);
    vxe::voxel::BlockStateId state = *first;
    int steps = 0;
    while (catalog.nextLayer(state) != vxe::voxel::kAirState) {
        state = catalog.nextLayer(state);
        ++steps;
    }
    CHECK(steps == 7);
    CHECK(state == *last);
    CHECK(catalog.isReplaceable(*first));
    CHECK_FALSE(catalog.isReplaceable(*catalog.stateId("snow[layers=3]")));
    CHECK(catalog.needsSupport(*first));
    CHECK(catalog.collisionHeight(*first) == doctest::Approx(2.0 / 16.0));
}

TEST_CASE("Aim boxes come from the kind of block or the block file") {
    const std::vector<BlockSource> sources = {
        {.file = "stone.toml",
         .text = "name = \"stone\"\ndisplay_name = \"S\"\nlayer = \"opaque\"\n[textures]\nall = \"s\""},
        {.file = "poppy.toml",
         .text = "name = \"poppy\"\ndisplay_name = \"P\"\nlayer = \"plant\"\n[textures]\nall = \"p\""},
        {.file = "torch.toml",
         .text = "name = \"torch\"\ndisplay_name = \"T\"\nlayer = \"plant\"\naim_box = [7, 0, 7, 9, 10, 9]\n"
                 "[textures]\nall = \"t\""},
        {.file = "slab.toml",
         .text = "name = \"slab\"\ndisplay_name = \"L\"\nlayer = \"opaque\"\nheight = 8\n[textures]\nall = "
                 "\"l\""},
    };
    const auto loaded = BlockCatalog::load(sources);
    REQUIRE(loaded.hasValue());
    const BlockCatalog& catalog = loaded.value();
    const auto box = [&](std::string_view name) { return catalog.aimBox(*catalog.stateId(name)); };
    CHECK(box("stone") == BlockCatalog::AimBox{0, 0, 0, 16, 16, 16});
    CHECK(box("poppy") == BlockCatalog::AimBox{2, 0, 2, 14, 13, 14});
    CHECK(box("torch") == BlockCatalog::AimBox{7, 0, 7, 9, 10, 9});
    CHECK(box("slab") == BlockCatalog::AimBox{0, 0, 0, 16, 8, 16});
    CHECK(
        errorOf(
            {{.file = "bad.toml",
              .text =
                  "name = \"bad\"\ndisplay_name = \"B\"\nlayer = \"opaque\"\naim_box = [4, 0, 4, 2, 16, 16]\n"
                  "[textures]\nall = \"b\""}})
            .find("aim_box") != std::string::npos);
}
