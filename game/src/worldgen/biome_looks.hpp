#pragma once

#include "common/block_catalog.hpp"
#include "worldgen/climate.hpp"

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <array>
#include <vector>

namespace vr {

// The shapes trees grow in. Terrain::stampTree builds them.
enum class TreeKind : vxe::u8 {
    Oak,
    BigOak,
    Birch,
    TallBirch,
    Spruce,
    Pine,
    DarkOak,
    PaleOak,
    Acacia,
    Jungle,
    GiantJungle,
    Azalea,
    Bush,
    TallSpruce,
    Palm,
    Olive,
    Maple,
    GoldMaple,
    Hemlock,
    Poplar,
    WeepingBirch,
    ForkedBirch,
};
inline constexpr vxe::usize kTreeKindCount = 22;

// What a kind of tree is made of. Some leaves are mixed with a second kind,
// so crowns are not one flat color.
struct TreeBlocks {
    vxe::voxel::BlockStateId log = 0;
    vxe::voxel::BlockStateId logX = 0; // Lying along X, for branches and fallen trunks.
    vxe::voxel::BlockStateId logZ = 0;
    vxe::voxel::BlockStateId leaves = 0;
    vxe::voxel::BlockStateId mixedLeaves = 0;
    vxe::f32 mixedShare = 0.0F; // 0 to 1.
};

struct TreeChoice {
    TreeKind kind = TreeKind::Oak;
    vxe::u16 weight = 0;
};

struct PlantChoice {
    vxe::voxel::BlockStateId state = 0;
    vxe::voxel::BlockStateId upper = 0; // The top half of a tall plant, or air.
    vxe::u16 weight = 0;
};

// Everything that makes a biome look like itself.
struct BiomeLook {
    vxe::voxel::BlockStateId top = 0;
    vxe::voxel::BlockStateId under = 0;
    vxe::voxel::BlockStateId floor = 0; // Under water.
    vxe::voxel::BlockStateId deep = 0;  // Below `under`, down to deepDepth; 0 for plain rock.
    vxe::voxel::BlockStateId patch = 0; // Replaces the top in patches.
    vxe::voxel::BlockStateId patch2 = 0;
    vxe::f32 patchShare = 0.0F; // 0 to 1 of the ground.
    vxe::f32 patch2Share = 0.0F;
    vxe::u8 underDepth = 3;
    vxe::u8 deepDepth = 0;
    bool banded = false; // Colored rock layers, as in badlands.
    bool dry = false;    // Water does little here: thin green along rivers.
    // What grows at water here, rooted in sand too. Oak means the biome's own trees.
    TreeKind waterTree = TreeKind::Oak;
    vxe::f32 trees = 0.0F;  // Chance of a tree per tree cell.
    vxe::f32 groves = 0.0F; // 0 is even, 1 gathers the trees into groups with clearings.
    vxe::u8 treeCount = 0;
    std::array<TreeChoice, 6> treeKinds{};
    vxe::f32 grass = 0.0F; // Share of the ground under grass and the like.
    vxe::u8 grassCount = 0;
    std::array<PlantChoice, 5> grassKinds{};
    vxe::f32 flowers = 0.0F; // Share of the ground in flower patches.
    vxe::u8 flowerCount = 0;
    std::array<PlantChoice, 7> flowerKinds{};
    vxe::voxel::BlockStateId boulder = 0;
    vxe::voxel::BlockStateId boulderMix = 0;
    vxe::f32 boulders = 0.0F; // Chance per feature cell.
    vxe::f32 fallenLogs = 0.0F;
    TreeKind logKind = TreeKind::Oak;
    // Snow layers lying on the open ground at most, 0 for a biome without snow.
    // Where it snows, the top of still water freezes too.
    vxe::u8 snowDepth = 0;
};

// What fallen snow does on a block (TerrainBlocks::snowRest).
enum class SnowRest : vxe::u8 {
    None,  // Slides off or melts: water, glass, slabs.
    Holds, // Lies on top: ground, rock, logs, leaves.
    Buries // Disappears under it: grass and flowers.
};

// The block states world generation places, from names in the catalog.
struct TerrainBlocks {
    vxe::voxel::BlockStateId stone = 0;
    vxe::voxel::BlockStateId granite = 0;
    vxe::voxel::BlockStateId andesite = 0;
    vxe::voxel::BlockStateId tuff = 0;
    vxe::voxel::BlockStateId calcite = 0;
    vxe::voxel::BlockStateId basalt = 0;
    vxe::voxel::BlockStateId gravel = 0;
    vxe::voxel::BlockStateId sand = 0;
    vxe::voxel::BlockStateId clay = 0;
    vxe::voxel::BlockStateId sandstone = 0;
    vxe::voxel::BlockStateId redSandstone = 0;
    vxe::voxel::BlockStateId packedMud = 0;
    vxe::voxel::BlockStateId water = 0;
    vxe::voxel::BlockStateId coal = 0;
    vxe::voxel::BlockStateId iron = 0;
    vxe::voxel::BlockStateId gold = 0;
    vxe::voxel::BlockStateId copper = 0;
    vxe::voxel::BlockStateId diamond = 0;
    vxe::voxel::BlockStateId emerald = 0;
    // The deep: deepslate below height 0, its ores (lapis and redstone only
    // come in deepslate), and lava in the deep caves.
    vxe::voxel::BlockStateId deepslate = 0;
    vxe::voxel::BlockStateId deepCoal = 0;
    vxe::voxel::BlockStateId deepIron = 0;
    vxe::voxel::BlockStateId deepGold = 0;
    vxe::voxel::BlockStateId deepCopper = 0;
    vxe::voxel::BlockStateId deepDiamond = 0;
    vxe::voxel::BlockStateId deepEmerald = 0;
    vxe::voxel::BlockStateId deepLapis = 0;
    vxe::voxel::BlockStateId deepRedstone = 0;
    vxe::voxel::BlockStateId lava = 0;
    vxe::voxel::BlockStateId floor = 0;
    std::array<vxe::voxel::BlockStateId, 9> soils{}; // What plants and trees root in.
    std::array<TreeBlocks, kTreeKindCount> trees{};
    std::array<PlantChoice, 7> waterside{};               // What crowds the banks of rivers and lakes.
    std::array<vxe::voxel::BlockStateId, 8> snowLayers{}; // snow[layers=1] to 8.
    vxe::voxel::BlockStateId ice = 0;
    vxe::voxel::BlockStateId packedIce = 0;
    std::vector<SnowRest> snowRest; // Per state.
    vxe::u8 watersideCount = 0;
    std::array<BiomeLook, kBiomeCount> biomes{};

    [[nodiscard]] bool isSoil(vxe::voxel::BlockStateId state) const noexcept {
        for (const vxe::voxel::BlockStateId soil : soils) {
            if (soil == state) {
                return state != vxe::voxel::kAirState;
            }
        }
        return false;
    }

    // Looks every state up by name; fails if the catalog lacks one.
    [[nodiscard]] static vxe::Result<TerrainBlocks> fromCatalog(const BlockCatalog& catalog);
};

} // namespace vr
