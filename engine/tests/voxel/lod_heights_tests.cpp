#include <vxe/voxel/lod_heights.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <vector>

using namespace vxe;

namespace {

// A tile whose every cell is one span from 0 up to `top`, except cell (cx, cz)
// which reaches `peak`.
std::shared_ptr<const voxel::LodTile> tileOf(voxel::LodTileKey key, i32 top, u32 peakX, u32 peakZ, i32 peak) {
    voxel::LodTileBuilder builder(key);
    for (u32 z = 0; z < 64; ++z) {
        for (u32 x = 0; x < 64; ++x) {
            const voxel::LodSpan span{.bottomY = 0, .topY = (x == peakX && z == peakZ) ? peak : top};
            builder.addCell(std::span(&span, 1));
        }
    }
    return std::make_shared<const voxel::LodTile>(std::move(builder).build());
}

} // namespace

TEST_CASE("Far terrain tiles fill a height grid, finer tiles first and the highest cell of each") {
    // A coarse tile (level 3, 8 block cells, 512 blocks) at height 20 under a
    // fine one (level 1, 2 block cells, 128 blocks) at height 40 with a peak.
    const auto coarse = tileOf({.level = 3, .x = 0, .z = 0}, 20, 99, 99, 20);
    const auto fine = tileOf({.level = 1, .x = 0, .z = 0}, 40, 5, 6, 90);
    const std::vector<std::shared_ptr<const voxel::LodTile>> tiles = {coarse, fine};
    constexpr u32 kCells = 32;
    std::vector<f32> heights(kCells * kCells);
    // The grid starts 16 blocks before the tiles: its first 2 cells stay unknown.
    voxel::fillLodHeights(tiles, -16, 0, kCells, 8, heights);
    CHECK(heights[0] == voxel::kNoLodHeight);
    CHECK(heights[1] == voxel::kNoLodHeight);
    // Block 0 to 7 of x and z: the fine tile, top face one above the top block.
    CHECK(heights[2] == doctest::Approx(41.0F));
    // The peak at fine cell (5, 6): blocks 10 to 11 and 12 to 13, grid cell (3, 1).
    CHECK(heights[(1 * kCells) + 3] == doctest::Approx(91.0F));
    // Beyond the fine tile's 128 blocks the coarse tile shows.
    CHECK(heights[(20 * kCells) + 20] == doctest::Approx(21.0F));
}
