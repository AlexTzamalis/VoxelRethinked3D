#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_storage.hpp>

#include <array>
#include <bitset>
#include <span>
#include <vector>

namespace vxe::voxel {

// The chunk and its 26 neighbors, in neighborBit order (index 13 is the
// chunk itself); null for neighbors that are not there.
using ChunkNeighborhood = std::array<const ChunkStorage*, 27>;

// Block columns of a neighborhood's 48 x 48 footprint that see open sky
// above the neighborhood's top: bit z * 48 + x, from its -X -Z corner. The
// caller works it out from column summaries (VoxelWorld owns the data).
using SkyOpenings = std::bitset<48 * 48>;

// True if any block in the neighborhood emits light. Cheap: only palettes
// are looked at.
[[nodiscard]] bool neighborhoodEmitsLight(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks);

// Computes the light of a chunk (Section 14.1, stages B and C) from the 48 x
// 48 x 48 blocks of its neighborhood: the chunk and its 26 neighbors.
//
// Block light: light emitting blocks (torches, lava, glowing blocks) light
// their surroundings, one level less per block travelled, more through
// blocks with a higher light opacity, up to 15 blocks. Fully opaque blocks
// stop it but still glow themselves if they emit.
//
// Sky light: full light (15) enters every open column at the top of the
// neighborhood and falls straight down through clear blocks without
// losing anything; the first block that dims it (leaves, water) takes off
// its opacity, and from there, and sideways and down into overhangs and
// caves, sky light spreads like block light, losing at least one level per
// block. Whatever stands above the neighborhood is only seen through the
// openings: a column is either open sky at the top of the box or dark.
//
// Light from 15 blocks away at most can reach a block, and every path out of
// the box and back is longer than that, so for the middle chunk the result is
// exact for the given openings. Computing a chunk from scratch makes adding
// and removing a light source, or a roof, the same thing, with no stale
// light left behind.
//
// Both floods go level by level from 15 down (a bucket queue), so each
// block is settled at its final level the first time it is taken out.
// Missing neighbors (above or below the world) count as open air without
// light sources.
//
// Thread safety: a ChunkLighter holds scratch memory (about 300 KB), so each
// worker thread uses its own. The inputs are read only.
class ChunkLighter {
public:
    ChunkLighter();

    // Writes the packed light (packLight) of the middle chunk to `out`, in
    // block order. Without `blockLight`, light sources are ignored.
    void compute(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks, const SkyOpenings& sky,
                 bool blockLight, std::span<u8, kChunkVolume> out);

private:
    static constexpr usize kCells = usize{48} * 48 * 48;

    void fillChunk(usize k, const ChunkStorage* chunk, const BlockRegistryView& blocks, bool blockLight);
    void addSource(u32 cell, u8 level);
    void seedSky(const SkyOpenings& sky);
    void spread();

    std::vector<u8> m_lightOf;  // Per state: opacity (low 4 bits), emitted light (high 4 bits).
    std::vector<u8> m_opacity;  // Light opacity of each cell; 15 stops light.
    std::vector<u8> m_level;    // Light of each cell so far (block, then sky).
    std::vector<u32> m_sources; // Light sources, to seed the block flood.
    std::vector<u8> m_sourceLevels;
    std::array<std::vector<u32>, 16> m_queue; // Cells to spread from, per level.
    std::array<BlockStateId, kChunkVolume> m_states{};
    std::array<u8, kChunkVolume> m_blockLight{};
};

} // namespace vxe::voxel
