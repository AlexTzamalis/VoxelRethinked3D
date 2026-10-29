#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_storage.hpp>

#include <array>
#include <span>
#include <vector>

namespace vxe::voxel {

// The chunk and its 26 neighbors, in neighborBit order (index 13 is the
// chunk itself); null for neighbors that are not there.
using ChunkNeighborhood = std::array<const ChunkStorage*, 27>;

// True if any block in the neighborhood emits light, so the chunk's block
// light can be other than 0. Cheap: only palettes are looked at. Most
// chunks have no light source within reach and skip lighting entirely.
[[nodiscard]] bool neighborhoodEmitsLight(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks);

// Block light (Section 14.1, stage B): light emitting blocks (torches, lava,
// glowing blocks) light their surroundings, one level less per block they
// travel through (more through blocks with a higher light opacity), up to 15
// blocks. Fully opaque blocks stop it but still glow themselves if they emit.
//
// A light source at most 15 blocks away can reach a block, so a chunk's block
// light depends only on the chunk and its 26 neighbors, the 48 x 48 x 48
// blocks around it: any path out of that box and back is longer than 15. So
// the light of one chunk is computed from scratch from its neighborhood.
// Placing and removing a light source are then the same thing (compute the
// chunks within reach again), and no stale light can be left behind, which
// incremental add and remove propagation has to work hard to avoid.
//
// The computation is a flood fill from every source at once, processed level
// by level from 15 down (a bucket queue), so each block is settled at its
// final level the first time it is taken out. Missing neighbors (above or
// below the world) count as open air with no light sources.
//
// Thread safety: a BlockLighter holds scratch memory (about 300 KB), so each
// worker thread uses its own. The inputs are read only.
class BlockLighter {
public:
    BlockLighter();

    // Writes the block light (0 to 15) of the chunk in the middle of
    // `chunks` to `out`, in block order.
    void compute(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks,
                 std::span<u8, kChunkVolume> out);

private:
    static constexpr usize kCells = usize{48} * 48 * 48; // The neighborhood box.

    void fillChunk(usize k, const ChunkStorage* chunk, const BlockRegistryView& blocks);
    void addSource(u32 cell, u8 level);
    void spread();

    std::vector<u8> m_attenuation;            // Light lost entering each cell; 15 or more stops it.
    std::vector<u8> m_level;                  // Light of each cell so far.
    std::array<std::vector<u32>, 16> m_queue; // Cells to spread from, per level.
    std::array<BlockStateId, kChunkVolume> m_states{};
};

} // namespace vxe::voxel
