#pragma once

#include "common/block_catalog.hpp"
#include "worldgen/planet.hpp"

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <array>
#include <optional>
#include <span>

namespace vr {

// The block states the terrain places.
struct TerrainBlocks {
    vxe::voxel::BlockStateId stone = 0;
    vxe::voxel::BlockStateId dirt = 0;
    vxe::voxel::BlockStateId grass = 0;
    vxe::voxel::BlockStateId sand = 0;
    vxe::voxel::BlockStateId gravel = 0;
    vxe::voxel::BlockStateId water = 0;
    vxe::voxel::BlockStateId snow = 0;
    vxe::voxel::BlockStateId coal = 0;
    vxe::voxel::BlockStateId iron = 0;
    vxe::voxel::BlockStateId gold = 0;
    vxe::voxel::BlockStateId floor = 0;
    vxe::voxel::BlockStateId log = 0;
    vxe::voxel::BlockStateId leaves = 0;

    // Looks every state up by name; fails if the catalog lacks one.
    [[nodiscard]] static vxe::Result<TerrainBlocks> fromCatalog(const BlockCatalog& catalog);
};

// What covers the ground of a column.
enum class Surface : vxe::u8 {
    Grass,
    Sand,
    Snow,
    Rock
};

// The ground at one position: its height in blocks and how steep it is, in
// blocks of height per block.
struct Ground {
    vxe::f64 height = 0.0;
    vxe::f64 slope = 0.0;
};

// The world's terrain (G3.3): the planet's heights, carved by an erosion
// filter into gullies that run downhill, with bare rock overhangs on steep
// faces. Both real chunks and far terrain read it, so they cannot disagree.
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
// Trees are gathered (Section 12.2): each chunk looks for tree origins in the
// cells around it and stamps the parts that fall inside itself, so a chunk
// never needs its neighbors.
//
// Deterministic for a seed and thread safe: everything here only reads, and
// the cache holds nothing that could not be computed again.
class Terrain {
public:
    static constexpr vxe::i64 kSeaLevel = 62;
    // The floor of foundation stone, at the bottom of the default height preset.
    static constexpr vxe::i64 kFloorY = -1024;
    // Trees: at most one per 7 x 7 block cell; leaves reach 2 blocks from
    // the trunk. Shared with the far terrain sampler, which places the same
    // trees (Section 13.4, features in LOD).
    static constexpr vxe::i64 kTreeCell = 7;
    static constexpr vxe::i64 kLeafRadius = 2;

    // Where the tree of a tree cell stands, from the seed alone (no terrain
    // lookup), or nothing if the cell has none. The tree only grows if the
    // ground there passes treeGrows().
    struct TreeSpot {
        vxe::i64 x = 0;
        vxe::i64 z = 0;
        vxe::i64 trunk = 4; // Blocks of trunk above the ground; the crown's top layer starts there.
    };
    [[nodiscard]] std::optional<TreeSpot> treeSpot(vxe::i64 cellX, vxe::i64 cellZ) const noexcept;
    [[nodiscard]] static constexpr bool treeGrows(Surface surface) noexcept {
        return surface == Surface::Grass;
    }

    Terrain(const TerrainBlocks& blocks, vxe::u64 seed, const PlanetPreset& preset = {}) noexcept;

    // The 4096 states of a chunk, in block order (LocalPos::index).
    void generate(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;

    // The height of the ground (the topmost solid block, before overhangs) and
    // its cover at a block column, exactly as chunks build them.
    [[nodiscard]] vxe::i64 surfaceHeight(vxe::i64 x, vxe::i64 z) const;
    [[nodiscard]] Surface surfaceAt(vxe::i64 x, vxe::i64 z) const;

    // The ground for far terrain cells of `cell` blocks: skips what a cell that
    // wide cannot show, which matters because a far tile is 4096 cells.
    [[nodiscard]] Ground farGround(vxe::f64 x, vxe::f64 z, vxe::f64 cell) const noexcept;

    // What covers ground of this height and slope, at a position.
    [[nodiscard]] Surface surfaceOf(vxe::f64 x, vxe::f64 z, const Ground& at) const noexcept;

    // A block column on dry, gentle land near the origin, for the player to
    // start on. Searches outwards, so it costs a few milliseconds.
    [[nodiscard]] vxe::BlockPos spawnColumn() const;

    [[nodiscard]] const Planet& planet() const noexcept { return m_planet; }

private:
    struct Column;

    // The chunk column's heights, from this thread's cache. The reference
    // holds until the next call on this thread.
    [[nodiscard]] const Column& column(vxe::i32 chunkX, vxe::i32 chunkZ) const;
    void buildColumn(vxe::i32 chunkX, vxe::i32 chunkZ, Column& column) const noexcept;

    // The ground with gullies of `shortest` blocks across and wider.
    [[nodiscard]] Ground ground(vxe::f64 x, vxe::f64 z, vxe::f64 shortest) const noexcept;
    void gullies(vxe::f64 x, vxe::f64 z, vxe::f64 shortest, vxe::f64 strength, Ground& out, vxe::f64& slopeX,
                 vxe::f64& slopeZ) const noexcept;

    [[nodiscard]] vxe::voxel::BlockStateId stoneAt(vxe::i64 x, vxe::i64 y, vxe::i64 z) const noexcept;
    void placeTrees(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;
    // Stamps the part of the tree of one cell (if it has one) that falls
    // inside the chunk starting at `origin`.
    void stampTree(const vxe::BlockPos& origin, vxe::i64 cellX, vxe::i64 cellZ,
                   std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;

    TerrainBlocks m_blocks;
    vxe::u64 m_seed;
    vxe::u64 m_identity; // Tells this terrain's cached columns from another's.
    Planet m_planet;
};

} // namespace vr
