#pragma once

#include "worldgen/biome_looks.hpp"
#include "worldgen/climate.hpp"
#include "worldgen/planet.hpp"
#include "worldgen/rivers.hpp"

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <optional>
#include <span>

namespace vr {

// The ground at one position, in blocks: how high, how steep (height per block
// walked), and what the planet says about the place, for choosing a biome.
struct Ground {
    vxe::f64 height = 0.0;
    vxe::f64 slope = 0.0;
    vxe::f64 mountain = 0.0;
    vxe::f64 rift = 0.0;
    vxe::f64 rough = 0.0;
    vxe::f64 continent = 0.0;
    vxe::f64 water = kNoWater; // Height of a river or lake surface, in blocks.
    vxe::f64 damp = 0.0;       // 0 to 1: how much water feeds the ground here.
    bool shore = false;        // On the flat strip beside a river.
};

// The world's terrain (G3.3, G3.4, G3.6): the planet's heights, carved by an
// erosion filter into gullies that run downhill, dressed by biome (BiomeLook)
// with soil, patches, rock layers, trees, plants, boulders and fallen logs.
// Both real chunks and far terrain read it, so they cannot disagree.
//
// The erosion filter works per point, so it needs no neighbors and no chunk
// order. Real erosion carves channels down the slope. Here each octave lays
// stripes along the direction of the slope, and passes on how it bent the
// ground, so the next, finer octave follows the new channels: that is what
// makes gullies branch like real ones. See gullies().
//
// Heights come from a grid of 4 blocks with per block detail on top. The chunks
// of one column share those heights in a cache per thread, which is what makes
// a chunk cheap: without it, each of the many chunks of a column would pay for
// the whole column.
//
// Trees and other features are gathered (Section 12.2): each chunk looks for
// feature origins in the cells around it and stamps the parts that fall inside
// itself, so a chunk never needs its neighbors. Plants grow column by column.
//
// Deterministic for a seed and thread safe: everything here only reads, and
// the cache holds nothing that could not be computed again.
class Terrain {
public:
    static constexpr vxe::i64 kSeaLevel = 62;
    // The floor of foundation stone, at the bottom of the default height preset.
    static constexpr vxe::i64 kFloorY = -1024;
    // Trees: at most one per 5 x 5 block cell, reaching at most 7 blocks from
    // the trunk. Shared with the far terrain sampler.
    static constexpr vxe::i64 kTreeCell = 5;
    static constexpr vxe::i64 kTreeReach = 7;

    // One tree: where it stands and what it is, from the seed and the biome.
    struct Tree {
        vxe::i64 x = 0;
        vxe::i64 z = 0;
        vxe::i64 crownX = 0; // The crown can lean away from the trunk.
        vxe::i64 crownZ = 0;
        vxe::i64 trunk = 4;      // Blocks of trunk above the ground.
        vxe::i64 height = 6;     // Blocks from the ground to the crown's top.
        vxe::i64 crownDepth = 4; // Blocks from the crown's top to its underside.
        vxe::i64 radius = 2;     // Of the crown.
        TreeKind kind = TreeKind::Oak;
        vxe::u64 seed = 0;
    };
    struct PlacedTree {
        Tree tree;
        vxe::i64 ground = 0;
    };
    // The tree of a tree cell, if one grows there.
    [[nodiscard]] std::optional<PlacedTree> treeAt(vxe::i64 cellX, vxe::i64 cellZ) const;

    // `rivers` off skips the drainage simulation, which takes a good part of a
    // second per area: only tests that have nothing to do with water do that.
    Terrain(const TerrainBlocks& blocks, vxe::u64 seed, const PlanetPreset& preset = {},
            bool rivers = true) noexcept;
    // Climate keeps a pointer to the planet inside this object.
    Terrain(const Terrain&) = delete;
    Terrain& operator=(const Terrain&) = delete;

    // The 4096 states of a chunk, in block order (LocalPos::index).
    void generate(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;

    // A block column, exactly as chunks build it: its ground height (before
    // overhangs), its biome and the block on top.
    [[nodiscard]] vxe::i64 surfaceHeight(vxe::i64 x, vxe::i64 z) const;
    [[nodiscard]] Biome biomeAt(vxe::i64 x, vxe::i64 z) const;
    [[nodiscard]] vxe::voxel::BlockStateId topBlock(vxe::i64 x, vxe::i64 z) const;
    // The surface of the water over a column: the sea, or a river or lake.
    [[nodiscard]] vxe::i64 waterLevel(vxe::i64 x, vxe::i64 z) const;

    // A far terrain cell of `cell` blocks. Skips what a cell that wide cannot
    // show, which matters because a far tile is 4096 cells. The climate comes
    // from the caller, who can blend it across a tile for much less.
    struct FarCell {
        vxe::i64 height = 0;
        Biome biome = Biome::Plains;
        vxe::voxel::BlockStateId top = 0;
        vxe::i64 water = kSeaLevel;
    };
    [[nodiscard]] FarCell farCell(vxe::f64 x, vxe::f64 z, vxe::f64 cell,
                                  const Climate::Field& field) const noexcept;

    // The ground for far terrain cells of `cell` blocks.
    [[nodiscard]] Ground farGround(vxe::f64 x, vxe::f64 z, vxe::f64 cell) const noexcept;

    // The ground and biome without rivers or the column cache: rough, but it
    // can never wait for a river tile, so the main thread can ask.
    [[nodiscard]] Ground roughGround(vxe::f64 x, vxe::f64 z) const noexcept;
    [[nodiscard]] Biome roughBiome(vxe::f64 x, vxe::f64 z) const noexcept;

    // How much of the ground lies under crowns in a biome, 0 to 1.
    [[nodiscard]] vxe::f64 treeCover(Biome biome) const noexcept;

    // A block column on dry, gentle land near the origin, for the player to
    // start on. Searches outwards, so it costs a few milliseconds.
    [[nodiscard]] vxe::BlockPos spawnColumn() const;

    // The nearest column of a biome, searching rings around a position up to
    // about 12 km away. Costs up to a second, so for tools and commands.
    [[nodiscard]] std::optional<vxe::BlockPos> findBiome(Biome biome, vxe::i64 x, vxe::i64 z) const;

    [[nodiscard]] const Planet& planet() const noexcept { return m_planet; }
    [[nodiscard]] const Climate& climate() const noexcept { return m_climate; }
    [[nodiscard]] const RiverNetwork& rivers() const noexcept { return m_rivers; }
    [[nodiscard]] const TerrainBlocks& blocks() const noexcept { return m_blocks; }

private:
    struct Column;

    // The chunk column's heights, from this thread's cache. The reference
    // holds until the next call on this thread.
    [[nodiscard]] const Column& column(vxe::i32 chunkX, vxe::i32 chunkZ) const;
    [[nodiscard]] const Column& columnAt(vxe::i64 x, vxe::i64 z) const;
    void buildColumn(vxe::i32 chunkX, vxe::i32 chunkZ, Column& column) const noexcept;

    // The ground with gullies of `shortest` blocks across and wider.
    [[nodiscard]] Ground ground(vxe::f64 x, vxe::f64 z, vxe::f64 shortest) const noexcept;
    void gullies(vxe::f64 x, vxe::f64 z, vxe::f64 shortest, vxe::f64 strength, Ground& out, vxe::f64& slopeX,
                 vxe::f64& slopeZ) const noexcept;

    // Cuts the rivers among `nearby` into this ground.
    static void carveInto(std::span<const RiverNetwork::Stretch> nearby, vxe::f64 x, vxe::f64 z,
                          Ground& at) noexcept;

    // The block on top of a column of this ground in this biome.
    [[nodiscard]] vxe::voxel::BlockStateId topOf(vxe::f64 x, vxe::f64 z, const Ground& at,
                                                 Biome biome) const noexcept;
    // The tree of a tree cell for a biome, before asking about its ground.
    [[nodiscard]] std::optional<Tree> treeIn(vxe::i64 cellX, vxe::i64 cellZ, Biome biome,
                                             vxe::f64 damp) const noexcept;

    [[nodiscard]] vxe::voxel::BlockStateId stoneAt(vxe::i64 x, vxe::i64 y, vxe::i64 z, vxe::f64 a,
                                                   vxe::f64 b) const noexcept;
    [[nodiscard]] vxe::voxel::BlockStateId bandAt(vxe::i64 x, vxe::i64 y, vxe::i64 z,
                                                  bool banded) const noexcept;
    [[nodiscard]] const PlantChoice* plantAt(vxe::i64 x, vxe::i64 z, const BiomeLook& look,
                                             vxe::f64 damp) const noexcept;

    void placeFeatures(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;
    void placeTrees(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;
    void placePlants(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;

    TerrainBlocks m_blocks;
    vxe::u64 m_seed;
    vxe::u64 m_identity; // Tells this terrain's cached columns from another's.
    Planet m_planet;
    Climate m_climate;
    RiverNetwork m_rivers;
    bool m_riversOn;
};

} // namespace vr
