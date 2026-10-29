#include "demo_lod_sampler.hpp"

#include <span>

namespace vr {

using vxe::i64;
using vxe::voxel::BlockFace;
using vxe::voxel::LodSpan;

namespace {

// Bumped whenever the demo terrain or this sampler changes what it makes,
// so cached tiles of older versions are thrown away.
constexpr vxe::u32 kDemoGeneratorVersion = 1;

} // namespace

DemoLodSampler::DemoLodSampler(const DemoTerrain& terrain, const DemoBlocks& blocks,
                               const vxe::voxel::BlockRegistryView& view, vxe::u64 seed) noexcept
    : m_terrain(terrain)
    , m_hash(vxe::voxel::lodWorldHash(seed, kDemoGeneratorVersion, 0)) {
    m_materials = {.grass = view.info(blocks.grass).textureLayer(BlockFace::PosY),
                   .grassSide = view.info(blocks.grass).textureLayer(BlockFace::PosX),
                   .sand = view.info(blocks.sand).textureLayer(BlockFace::PosY),
                   .snow = view.info(blocks.snow).textureLayer(BlockFace::PosY),
                   .water = view.info(blocks.water).textureLayer(BlockFace::PosY),
                   .stone = view.info(blocks.stone).textureLayer(BlockFace::PosX)};
}

bool DemoLodSampler::sampleTile(const vxe::voxel::LodTileKey& key,
                                const vxe::voxel::LodSampleOptions& /*options*/,
                                vxe::voxel::LodTileBuilder& builder, const std::atomic<bool>& cancelled) {
    const i64 cell = key.cellSize();
    for (vxe::i32 cz = 0; cz < vxe::voxel::kLodTileCells; ++cz) {
        if (cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
        for (vxe::i32 cx = 0; cx < vxe::voxel::kLodTileCells; ++cx) {
            const i64 x = key.originX() + (cx * cell) + (cell / 2);
            const i64 z = key.originZ() + (cz * cell) + (cell / 2);
            const i64 height = m_terrain.surfaceHeight(x, z);
            LodSpan span{.bottomY = 0, .topY = static_cast<vxe::i32>(height)};
            if (height < DemoTerrain::kSeaLevel) {
                // A lake or the sea: the water surface is what shows.
                span.topY = static_cast<vxe::i32>(DemoTerrain::kSeaLevel);
                span.topMaterial = m_materials.water;
                span.sideMaterial = m_materials.water;
            } else if (height <= DemoTerrain::kSeaLevel + 1) {
                span.topMaterial = m_materials.sand;
                span.sideMaterial = m_materials.sand;
            } else if (height > 100) {
                span.topMaterial = m_materials.snow;
                span.sideMaterial = m_materials.stone;
            } else {
                span.topMaterial = m_materials.grass;
                span.sideMaterial = m_materials.grassSide;
            }
            builder.addCell(std::span(&span, 1));
        }
    }
    return true;
}

} // namespace vr
