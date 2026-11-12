#include "vxe/voxel/lod_heights.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <vector>

namespace vxe::voxel {

void fillLodHeights(std::span<const std::shared_ptr<const LodTile>> tiles, i64 originX, i64 originZ,
                    u32 cells, i32 cellBlocks, std::span<f32> out) {
    VXE_PROFILE_FUNCTION();
    VXE_ASSERT(out.size() == usize{cells} * cells && cellBlocks > 0,
               "fillLodHeights needs a cells x cells grid");
    std::ranges::fill(out, kNoLodHeight);
    // The level that wrote each cell: a finer tile wins over a coarser one,
    // and cells of one level that share a grid cell keep the highest.
    std::vector<u8> levelOf(out.size(), u8{255});
    const i64 size = i64{cells} * cellBlocks;
    for (const std::shared_ptr<const LodTile>& tile : tiles) {
        if (tile == nullptr) {
            continue;
        }
        const LodTileKey& key = tile->key();
        const i64 tileX = key.originX() - originX;
        const i64 tileZ = key.originZ() - originZ;
        if (tileX >= size || tileZ >= size || tileX + key.tileSize() <= 0 || tileZ + key.tileSize() <= 0) {
            continue;
        }
        const i64 cell = key.cellSize();
        for (u32 cz = 0; cz < static_cast<u32>(kLodTileCells); ++cz) {
            const i64 z0 = tileZ + (i64{cz} * cell);
            if (z0 + cell <= 0 || z0 >= size) {
                continue;
            }
            for (u32 cx = 0; cx < static_cast<u32>(kLodTileCells); ++cx) {
                const i64 x0 = tileX + (i64{cx} * cell);
                if (x0 + cell <= 0 || x0 >= size) {
                    continue;
                }
                const std::span<const LodSpan> spans = tile->cell(cx, cz);
                if (spans.empty()) {
                    continue;
                }
                const auto top = static_cast<f32>(spans.front().topY + 1); // The top face of the block.
                // The grid cells this tile cell covers.
                const i64 gx0 = std::max<i64>(x0, 0) / cellBlocks;
                const i64 gz0 = std::max<i64>(z0, 0) / cellBlocks;
                const i64 gx1 = std::min<i64>(x0 + cell - 1, size - 1) / cellBlocks;
                const i64 gz1 = std::min<i64>(z0 + cell - 1, size - 1) / cellBlocks;
                for (i64 gz = gz0; gz <= gz1; ++gz) {
                    for (i64 gx = gx0; gx <= gx1; ++gx) {
                        const auto index = static_cast<usize>((gz * cells) + gx);
                        if (key.level < levelOf[index]) {
                            levelOf[index] = key.level;
                            out[index] = top;
                        } else if (key.level == levelOf[index]) {
                            out[index] = std::max(out[index], top);
                        }
                    }
                }
            }
        }
    }
}

} // namespace vxe::voxel
