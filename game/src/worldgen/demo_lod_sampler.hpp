#pragma once

#include "worldgen/demo_terrain.hpp"

#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/lod_manager.hpp>

namespace vr {

// Far terrain for the demo world (until the real column sampler, G3.6):
// samples DemoTerrain's height at the middle of each cell and turns it into
// one span, colored like the blocks the demo terrain puts on top (grass,
// sand on beaches, snow above 100, water over ground below sea level).
// Materials are the blocks' texture layers, so the renderer's material table
// is the per layer color table.
//
// Thread safety: sampleTile only reads; any number of workers at once.
class DemoLodSampler final : public vxe::voxel::LodTileSampler {
public:
    DemoLodSampler(const DemoTerrain& terrain, const DemoBlocks& blocks,
                   const vxe::voxel::BlockRegistryView& view, vxe::u64 seed) noexcept;

    bool sampleTile(const vxe::voxel::LodTileKey& key, const vxe::voxel::LodSampleOptions& options,
                    vxe::voxel::LodTileBuilder& builder, const std::atomic<bool>& cancelled) override;
    [[nodiscard]] vxe::u64 worldHash() const override { return m_hash; }

private:
    struct Materials {
        vxe::u16 grass = 0;
        vxe::u16 grassSide = 0;
        vxe::u16 sand = 0;
        vxe::u16 snow = 0;
        vxe::u16 water = 0;
        vxe::u16 stone = 0;
    };

    const DemoTerrain& m_terrain;
    Materials m_materials;
    vxe::u64 m_hash;
};

} // namespace vr
