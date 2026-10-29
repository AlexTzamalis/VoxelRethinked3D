#pragma once

#include "common/block_catalog.hpp"

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <span>

namespace vr {

// The block states the demo terrain places.
struct DemoBlocks {
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
    [[nodiscard]] static vxe::Result<DemoBlocks> fromCatalog(const BlockCatalog& catalog);
};

// Terrain for the temporary demo world (until world generation, G3): hills
// and low mountains from a few octaves of value noise, beaches and lakes at
// sea level 62, snow above 100, dirt under grass, ores and gravel pockets in
// the stone, caves from 3D noise, and a floor of foundation stone at the
// bottom of the world. Trees are gathered (Section 12.2): each chunk looks
// for tree origins in the cells around it and stamps the parts that fall
// inside itself, so a chunk never needs its neighbors.
//
// Deterministic for a seed and thread safe: generate() only reads.
class DemoTerrain {
public:
    static constexpr vxe::i64 kSeaLevel = 62;

    DemoTerrain(const DemoBlocks& blocks, vxe::u64 seed) noexcept
        : m_blocks(blocks)
        , m_seed(seed) {}

    // The 4096 states of a chunk, in block order (LocalPos::index).
    void generate(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;
    // The height of the ground (the topmost solid block) at a block column.
    [[nodiscard]] vxe::i64 surfaceHeight(vxe::i64 x, vxe::i64 z) const noexcept;

private:
    // The state of one block before trees, given its column's ground height.
    [[nodiscard]] vxe::voxel::BlockStateId blockAt(vxe::i64 x, vxe::i64 y, vxe::i64 z,
                                                   vxe::i64 height) const noexcept;
    [[nodiscard]] vxe::voxel::BlockStateId stoneAt(vxe::i64 x, vxe::i64 y, vxe::i64 z) const noexcept;
    void placeTrees(vxe::ChunkPos chunk, std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;
    // Stamps the part of the tree of one cell (if it has one) that falls
    // inside the chunk starting at `origin`.
    void stampTree(const vxe::BlockPos& origin, vxe::i64 cellX, vxe::i64 cellZ,
                   std::span<vxe::voxel::BlockStateId, vxe::kChunkVolume> out) const;

    DemoBlocks m_blocks;
    vxe::u64 m_seed;
};

} // namespace vr
