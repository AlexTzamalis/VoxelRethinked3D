#pragma once

#include <vxe/core/types.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_storage.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <array>
#include <span>
#include <vector>

// Turning real blocks and finer LOD cells into coarser LOD cells (Section
// 13.6): edits the player makes reach the far view by being downsampled
// into level 1 cells, then level by level up the quadtree.
namespace vxe::voxel {

// Level 1 cells along each side of a chunk column (16 blocks, 2 per cell).
inline constexpr i32 kLodCellsPerChunk = kChunkSize / 2;
inline constexpr usize kLodCellsPerColumn = usize{kLodCellsPerChunk} * kLodCellsPerChunk;

// Keeps at most `maxSpans` spans by joining the two neighboring spans with
// the smallest gap between them, again and again (a thin cave matters less
// than a tall overhang). The joined span keeps the upper one's top.
void limitLodSpans(std::vector<LodSpan>& spans, usize maxSpans);

// The level 1 cells of one chunk column, from its loaded chunks: chunk y of
// chunks[i] is firstChunkY + i (null for chunks that are not loaded). A
// block counts as terrain if any of the 2 x 2 columns of a cell has a
// visible block there (not RenderLayer::Invisible); runs of terrain become
// spans, with the top and side materials of the topmost block of the run
// (its +Y and +X texture layers, the LOD material convention). Only the
// height of the given chunks is known; `out` gets the spans inside it, cell
// (x, z) at index z * 8 + x.
void downsampleChunkColumn(std::span<const ChunkStorage* const> chunks, i32 firstChunkY,
                           const BlockRegistryView& blocks, usize maxSpans,
                           std::array<std::vector<LodSpan>, kLodCellsPerColumn>& out);

// Replaces the part of a cell's spans between blocks `low` and `high`
// (inclusive) with `inside`, keeping what lies outside (the world above and
// below the loaded chunks), then limits the result.
void replaceLodSpanRange(std::span<const LodSpan> old, std::span<const LodSpan> inside, i64 low, i64 high,
                         usize maxSpans, std::vector<LodSpan>& out);

// One cell of the next level from the 4 cells it covers: terrain wherever
// any child has terrain, materials and light from the child whose span
// reaches highest within each run.
void downsampleLodCells(std::array<std::span<const LodSpan>, 4> children, usize maxSpans,
                        std::vector<LodSpan>& out);

// Rewrites the cells of `parent` that cover `child` (a 32 x 32 quadrant)
// from the child's cells.
void downsampleLodTile(const LodTile& child, LodTile& parent, usize maxSpans);

} // namespace vxe::voxel
