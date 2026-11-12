#pragma once

#include <vxe/core/types.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <memory>
#include <span>

namespace vxe::voxel {

// Below every terrain: a cell with nothing known.
inline constexpr f32 kNoLodHeight = -1.0e9F;

// A square grid of terrain heights from far terrain tiles, for long shadows
// (render::SunlightShadows): `cells` x `cells` cells of `cellBlocks` blocks,
// cell (0, 0) at block column (originX, originZ), row by row along x. Each
// cell holds the top of the highest span the finest tile over it knows (the
// highest of several finer cells), or kNoLodHeight where no tile reached.
//
// Thread safety: tiles are read only, so any thread; meant for a job.
void fillLodHeights(std::span<const std::shared_ptr<const LodTile>> tiles, i64 originX, i64 originZ,
                    u32 cells, i32 cellBlocks, std::span<f32> out);

} // namespace vxe::voxel
