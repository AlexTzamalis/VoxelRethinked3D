#pragma once

#include "worldgen/terrain.hpp"

#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/lod_manager.hpp>

#include <array>
#include <span>
#include <vector>

namespace vr {

// Far terrain. It has to look like the real chunks from far away (Section
// 13.4, features in LOD), so it samples the same terrain and the same trees:
//
//   Ground   Terrain's height and top block at the middle of each cell (or water
//            over ground below sea level), so biomes show from far away. Cells
//            of up to 4 blocks read the very columns chunks use; wider ones ask
//            farCell, with climate blended across the tile.
//   Trees    in levels 1 and 2 (cells of 2 and 4 blocks) every tree is placed:
//            a cell at least half under a crown gets a crown span, floating
//            above the ground or joined to it, in that tree's leaves. In
//            coarser levels a tree is smaller than a cell, so the biome's tree
//            cover tints the grass towards leaves (in eighths), and dense
//            forest raises the cell to the crowns' height.
//
// Materials are texture layers (the LOD material convention), followed by
// LOD only materials: water as it looks over its dim floor, and grass mixed
// with leaves in eighths. materialColors() builds the whole color table from
// the textures' average colors.
//
// Thread safety: sampleTile only reads; any number of workers at once.
class TerrainLodSampler final : public vxe::voxel::LodTileSampler {
public:
    TerrainLodSampler(const Terrain& terrain, const vxe::voxel::BlockRegistryView& view, vxe::u16 layerCount,
                      vxe::u64 seed) noexcept;

    bool sampleTile(const vxe::voxel::LodTileKey& key, const vxe::voxel::LodSampleOptions& options,
                    vxe::voxel::LodTileBuilder& builder, const std::atomic<bool>& cancelled) override;
    [[nodiscard]] vxe::u64 worldHash() const override { return m_hash; }

    // The LOD material table from the average color of each texture layer
    // (RGBA8 sRGB): the layers themselves, then the LOD only materials.
    [[nodiscard]] std::vector<vxe::u32> materialColors(std::span<const vxe::u32> layerColors) const;

private:
    // Leaves cover 1/8 to 7/8 of a cell: grassLeaves[k - 1]; 8/8 is the
    // leaves layer itself.
    static constexpr vxe::u32 kBlendSteps = 8;

    struct Materials {
        vxe::u16 grass = 0;
        vxe::u16 sand = 0;
        vxe::u16 water = 0;
        vxe::u16 leaves = 0;
        vxe::u16 waterOverFloor = 0;                         // LOD only.
        std::array<vxe::u16, kBlendSteps - 1> grassLeaves{}; // LOD only.
    };
    // The crowns over each block column of the tile (levels 1 and 2).
    struct Crowns {
        std::vector<vxe::i32> top;
        std::vector<vxe::i32> bottom;
        std::vector<vxe::u16> material;
    };

    [[nodiscard]] vxe::voxel::LodSpan ground(vxe::i64 height, vxe::voxel::BlockStateId top) const noexcept;
    void placeCrowns(const vxe::voxel::LodTileKey& key, Crowns& crowns) const;
    void addFineCell(vxe::voxel::LodSpan base, const Crowns& crowns, vxe::u32 cx, vxe::u32 cz, vxe::i64 cell,
                     vxe::u32 maxSpans, std::vector<vxe::voxel::LodSpan>& spans) const;
    void addCoarseCell(vxe::voxel::LodSpan base, vxe::i64 height, vxe::f64 cover, vxe::i64 bump,
                       std::vector<vxe::voxel::LodSpan>& spans) const;
    [[nodiscard]] vxe::u16 grassWithLeaves(vxe::u32 eighths) const noexcept;

    const Terrain& m_terrain;
    const vxe::voxel::BlockRegistryView* m_view;
    Materials m_materials;
    vxe::u64 m_hash = 0;
};

} // namespace vr
