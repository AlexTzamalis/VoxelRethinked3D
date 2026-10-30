#include "terrain_lod_sampler.hpp"

#include <vxe/core/hash.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vr {

using vxe::f64;
using vxe::i32;
using vxe::i64;
using vxe::u32;
using vxe::usize;
using vxe::voxel::BlockFace;
using vxe::voxel::LodSpan;

namespace {

// Bumped whenever the terrain or this sampler changes what it makes, so cached
// tiles of older versions are thrown away.
constexpr vxe::u32 kGeneratorVersion = 4;
constexpr i32 kNoCrown = std::numeric_limits<i32>::min();
// Leaf blocks of one crown seen from above: 5 x 5 without the 4 corners.
constexpr f64 kCrownArea = 21.0;
// How far crowns stand above the ground on average: a trunk of 4 to 6
// blocks and the crown's top layer above it.
constexpr i64 kCrownAboveGround = 6;
// Real water lets 200 of 255 parts of its color through (the water texture's
// alpha); the floor below it is in the dark (sky light drops 2 per block of
// water), so it adds little.
constexpr f64 kFloorThroughWater = 0.25;

i64 floorDiv(i64 value, i64 divisor) noexcept {
    return value >= 0 ? value / divisor : ((value + 1) / divisor) - 1;
}

// sRGB colors mixed the way the GPU blends them: in linear light.
f64 toLinear(u32 channel) noexcept {
    const f64 c = static_cast<f64>(channel) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

u32 toSrgb(f64 linear) noexcept {
    const f64 c = std::clamp(linear, 0.0, 1.0);
    const f64 s = c <= 0.0031308 ? c * 12.92 : (1.055 * std::pow(c, 1.0 / 2.4)) - 0.055;
    return static_cast<u32>(std::lround(s * 255.0));
}

// a * weightA + b * weightB per color channel, opaque.
u32 blend(u32 a, f64 weightA, u32 b, f64 weightB) noexcept {
    u32 out = 0xFF000000U;
    for (u32 channel = 0; channel < 3; ++channel) {
        const u32 shift = channel * 8U;
        const f64 value =
            (toLinear((a >> shift) & 0xFFU) * weightA) + (toLinear((b >> shift) & 0xFFU) * weightB);
        out |= toSrgb(value) << shift;
    }
    return out;
}

} // namespace

TerrainLodSampler::TerrainLodSampler(const Terrain& terrain, const TerrainBlocks& blocks,
                                     const vxe::voxel::BlockRegistryView& view, vxe::u16 layerCount,
                                     vxe::u64 seed) noexcept
    : m_terrain(terrain) {
    m_materials.grass = view.info(blocks.grass).textureLayer(BlockFace::PosY);
    m_materials.grassSide = view.info(blocks.grass).textureLayer(BlockFace::PosX);
    m_materials.sand = view.info(blocks.sand).textureLayer(BlockFace::PosY);
    m_materials.snow = view.info(blocks.snow).textureLayer(BlockFace::PosY);
    m_materials.water = view.info(blocks.water).textureLayer(BlockFace::PosY);
    m_materials.stone = view.info(blocks.stone).textureLayer(BlockFace::PosX);
    m_materials.leaves = view.info(blocks.leaves).textureLayer(BlockFace::PosY);
    m_materials.waterOverFloor = layerCount;
    for (u32 k = 0; k + 1 < kBlendSteps; ++k) {
        m_materials.grassLeaves.at(k) = static_cast<vxe::u16>(layerCount + 1 + k);
    }
    // Cached tiles store these numbers, and a new block or texture moves them.
    // Part of the hash, so old tiles are sampled again instead of showing the
    // wrong materials.
    m_hash = vxe::voxel::lodWorldHash(seed, kGeneratorVersion,
                                      vxe::hashBytes(std::as_bytes(std::span(&m_materials, 1))));
}

std::vector<u32> TerrainLodSampler::materialColors(std::span<const u32> layerColors) const {
    std::vector<u32> colors(layerColors.begin(), layerColors.end());
    const auto layer = [&](vxe::u16 index) { return index < colors.size() ? colors[index] : 0xFFFF00FFU; };
    colors.resize(static_cast<usize>(m_materials.grassLeaves.back()) + 1, 0xFFFF00FFU);
    const u32 water = layer(m_materials.water);
    const f64 alpha = static_cast<f64>(water >> 24U) / 255.0;
    colors.at(m_materials.waterOverFloor) =
        blend(water, alpha, layer(m_materials.sand), (1.0 - alpha) * kFloorThroughWater);
    for (u32 k = 1; k < kBlendSteps; ++k) {
        const f64 leaves = static_cast<f64>(k) / kBlendSteps;
        colors.at(m_materials.grassLeaves.at(k - 1)) =
            blend(layer(m_materials.leaves), leaves, layer(m_materials.grass), 1.0 - leaves);
    }
    return colors;
}

vxe::u16 TerrainLodSampler::grassWithLeaves(u32 eighths) const noexcept {
    if (eighths == 0) {
        return m_materials.grass;
    }
    return eighths >= kBlendSteps ? m_materials.leaves : m_materials.grassLeaves.at(eighths - 1);
}

// The ground span of a column with this height.
LodSpan TerrainLodSampler::ground(i64 height, Surface surface) const noexcept {
    LodSpan span{.bottomY = 0, .topY = static_cast<i32>(height)};
    if (height < Terrain::kSeaLevel) {
        // A lake or the sea: the water surface is what shows.
        span.topY = static_cast<i32>(Terrain::kSeaLevel);
        span.topMaterial = m_materials.waterOverFloor;
        span.sideMaterial = m_materials.waterOverFloor;
        return span;
    }
    switch (surface) {
    case Surface::Sand:
        span.topMaterial = m_materials.sand;
        span.sideMaterial = m_materials.sand;
        break;
    case Surface::Snow:
        span.topMaterial = m_materials.snow;
        span.sideMaterial = m_materials.stone;
        break;
    case Surface::Rock:
        span.topMaterial = m_materials.stone;
        span.sideMaterial = m_materials.stone;
        break;
    case Surface::Grass:
        span.topMaterial = m_materials.grass;
        span.sideMaterial = m_materials.grassSide;
        break;
    }
    return span;
}

bool TerrainLodSampler::sampleTile(const vxe::voxel::LodTileKey& key,
                                   const vxe::voxel::LodSampleOptions& options,
                                   vxe::voxel::LodTileBuilder& builder, const std::atomic<bool>& cancelled) {
    const i64 cell = key.cellSize();
    thread_local std::vector<i64> heights;
    thread_local std::vector<Surface> surfaces;
    heights.resize(vxe::voxel::kLodCellCount);
    surfaces.resize(vxe::voxel::kLodCellCount);
    for (u32 cz = 0; cz < vxe::voxel::kLodTileCells; ++cz) {
        if (cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
        for (u32 cx = 0; cx < vxe::voxel::kLodTileCells; ++cx) {
            const i64 x = key.originX() + (cx * cell) + (cell / 2);
            const i64 z = key.originZ() + (cz * cell) + (cell / 2);
            const u32 index = (cz * vxe::voxel::kLodTileCells) + cx;
            if (cell <= 4) {
                heights[index] = m_terrain.surfaceHeight(x, z);
                surfaces[index] = m_terrain.surfaceAt(x, z);
            } else {
                const auto wx = static_cast<f64>(x);
                const auto wz = static_cast<f64>(z);
                const Ground at = m_terrain.farGround(wx, wz, static_cast<f64>(cell));
                heights[index] = static_cast<i64>(std::floor(at.height));
                surfaces[index] = m_terrain.surfaceOf(wx, wz, at);
            }
        }
    }
    // Levels 1 and 2 place every crown; coarser levels count trees per cell.
    const bool fine = cell <= 4;
    thread_local Crowns crowns;
    thread_local std::vector<u32> counts;
    if (fine) {
        placeCrowns(key, crowns);
    } else {
        counts.assign(vxe::voxel::kLodCellCount, 0);
        countTrees(key, surfaces, counts);
    }
    std::vector<LodSpan> spans;
    for (u32 cz = 0; cz < vxe::voxel::kLodTileCells; ++cz) {
        for (u32 cx = 0; cx < vxe::voxel::kLodTileCells; ++cx) {
            spans.clear();
            const u32 index = (cz * vxe::voxel::kLodTileCells) + cx;
            const LodSpan base = ground(heights[index], surfaces[index]);
            if (fine) {
                addFineCell(base, crowns, cx, cz, cell, options.maxSpansPerCell, spans);
            } else {
                addCoarseCell(base, heights[index], counts[index], cell, spans);
            }
            builder.addCell(spans);
        }
    }
    return true;
}

// Every crown that reaches into the tile, block column by block column: the
// highest top and lowest bottom of leaves over each column.
void TerrainLodSampler::placeCrowns(const vxe::voxel::LodTileKey& key, Crowns& crowns) const {
    const i64 size = key.tileSize();
    crowns.top.assign(static_cast<usize>(size * size), kNoCrown);
    crowns.bottom.assign(static_cast<usize>(size * size), std::numeric_limits<i32>::max());
    constexpr i64 kReach = Terrain::kLeafRadius;
    const i64 firstX = floorDiv(key.originX() - kReach, Terrain::kTreeCell);
    const i64 lastX = floorDiv(key.originX() + size + kReach, Terrain::kTreeCell);
    const i64 firstZ = floorDiv(key.originZ() - kReach, Terrain::kTreeCell);
    const i64 lastZ = floorDiv(key.originZ() + size + kReach, Terrain::kTreeCell);
    for (i64 cellZ = firstZ; cellZ <= lastZ; ++cellZ) {
        for (i64 cellX = firstX; cellX <= lastX; ++cellX) {
            const auto spot = m_terrain.treeSpot(cellX, cellZ);
            if (!spot) {
                continue;
            }
            const i64 groundY = m_terrain.surfaceHeight(spot->x, spot->z);
            if (!Terrain::treeGrows(m_terrain.surfaceAt(spot->x, spot->z))) {
                continue;
            }
            // The crown as Terrain::stampTree builds it: two 5 x 5 layers
            // without corners, then two 3 x 3 layers on top.
            const i64 top = groundY + spot->trunk;
            for (i64 dz = -kReach; dz <= kReach; ++dz) {
                for (i64 dx = -kReach; dx <= kReach; ++dx) {
                    if (std::abs(dx) == kReach && std::abs(dz) == kReach) {
                        continue;
                    }
                    const i64 x = spot->x + dx - key.originX();
                    const i64 z = spot->z + dz - key.originZ();
                    if (x < 0 || z < 0 || x >= size || z >= size) {
                        continue;
                    }
                    const bool inner = std::abs(dx) <= 1 && std::abs(dz) <= 1;
                    const auto index = static_cast<usize>((z * size) + x);
                    crowns.top[index] =
                        std::max(crowns.top[index], static_cast<i32>(inner ? top + 1 : top - 1));
                    crowns.bottom[index] = std::min(crowns.bottom[index], static_cast<i32>(top - 2));
                }
            }
        }
    }
}

// Coarse levels: the trees whose spot lies in each cell, where the cell's
// ground lets them grow.
void TerrainLodSampler::countTrees(const vxe::voxel::LodTileKey& key, std::span<const Surface> surfaces,
                                   std::span<u32> counts) const {
    const i64 size = key.tileSize();
    const i64 cell = key.cellSize();
    const i64 firstX = floorDiv(key.originX(), Terrain::kTreeCell);
    const i64 lastX = floorDiv(key.originX() + size - 1, Terrain::kTreeCell);
    const i64 firstZ = floorDiv(key.originZ(), Terrain::kTreeCell);
    const i64 lastZ = floorDiv(key.originZ() + size - 1, Terrain::kTreeCell);
    for (i64 cellZ = firstZ; cellZ <= lastZ; ++cellZ) {
        for (i64 cellX = firstX; cellX <= lastX; ++cellX) {
            const auto spot = m_terrain.treeSpot(cellX, cellZ);
            if (!spot) {
                continue;
            }
            const i64 x = (spot->x - key.originX()) / cell;
            const i64 z = (spot->z - key.originZ()) / cell;
            if (spot->x < key.originX() || spot->z < key.originZ() || x >= vxe::voxel::kLodTileCells ||
                z >= vxe::voxel::kLodTileCells) {
                continue;
            }
            const auto index = static_cast<usize>((z * vxe::voxel::kLodTileCells) + x);
            if (Terrain::treeGrows(surfaces[index])) {
                ++counts[index];
            }
        }
    }
}

void TerrainLodSampler::addFineCell(LodSpan base, const Crowns& crowns, u32 cx, u32 cz, i64 cell,
                                    u32 maxSpans, std::vector<LodSpan>& spans) const {
    const i64 size = cell * vxe::voxel::kLodTileCells;
    i32 crownTop = kNoCrown;
    i32 crownBottom = std::numeric_limits<i32>::max();
    i64 covered = 0;
    for (i64 z = cz * cell; z < (cz + 1) * cell; ++z) {
        for (i64 x = cx * cell; x < (cx + 1) * cell; ++x) {
            const auto index = static_cast<usize>((z * size) + x);
            if (crowns.top[index] != kNoCrown) {
                ++covered;
                crownTop = std::max(crownTop, crowns.top[index]);
                crownBottom = std::min(crownBottom, crowns.bottom[index]);
            }
        }
    }
    if (covered * 2 < cell * cell) {
        // Less than half under leaves: the grass shows, a little darker.
        if (covered > 0 && base.topMaterial == m_materials.grass) {
            base.topMaterial = grassWithLeaves(static_cast<u32>((covered * kBlendSteps) / (cell * cell)));
        }
        spans.push_back(base);
        return;
    }
    if (crownBottom <= base.topY + 1 || maxSpans < 2) {
        base.topY = std::max(base.topY, crownTop);
        base.topMaterial = m_materials.leaves;
        base.sideMaterial = m_materials.leaves;
        spans.push_back(base);
        return;
    }
    spans.push_back(LodSpan{.bottomY = crownBottom,
                            .topY = crownTop,
                            .topMaterial = m_materials.leaves,
                            .sideMaterial = m_materials.leaves});
    spans.push_back(base);
}

void TerrainLodSampler::addCoarseCell(LodSpan base, i64 height, u32 trees, i64 cell,
                                      std::vector<LodSpan>& spans) const {
    if (trees > 0 && base.topMaterial == m_materials.grass) {
        // The share of the cell under crowns, in eighths.
        const f64 share =
            std::min(1.0, (static_cast<f64>(trees) * kCrownArea) / static_cast<f64>(cell * cell));
        const auto eighths = static_cast<u32>(std::lround(share * kBlendSteps));
        base.topMaterial = grassWithLeaves(eighths);
        if (eighths >= kBlendSteps - 1) {
            // Dense forest: from far away the crowns are the ground.
            base.topY = static_cast<i32>(height + kCrownAboveGround);
            base.sideMaterial = m_materials.leaves;
        }
    }
    spans.push_back(base);
}

} // namespace vr
