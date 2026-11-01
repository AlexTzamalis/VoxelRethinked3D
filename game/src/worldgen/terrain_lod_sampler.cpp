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
constexpr vxe::u32 kGeneratorVersion = 20;
constexpr i32 kNoCrown = std::numeric_limits<i32>::min();
// How far crowns stand above the ground on average.
constexpr i64 kCrownAboveGround = 8;
// Real crowns let the sky through and catch light on every leaf. A far crown
// is one solid surface, so its color has to be lifted to look the same.
constexpr f64 kFarLeafGain = 1.5;
// Climate is blended across a tile from a grid this many cells apart.
constexpr u32 kClimateStride = 8;
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

TerrainLodSampler::TerrainLodSampler(const Terrain& terrain, const vxe::voxel::BlockRegistryView& view,
                                     vxe::u16 layerCount, vxe::u64 seed) noexcept
    : m_terrain(terrain)
    , m_view(&view) {
    const TerrainBlocks& blocks = terrain.blocks();
    m_materials.grass = view.info(blocks.soils[0]).textureLayer(BlockFace::PosY);
    m_materials.sand = view.info(blocks.sand).textureLayer(BlockFace::PosY);
    m_materials.water = view.info(blocks.water).textureLayer(BlockFace::PosY);
    m_materials.leaves = view.info(blocks.trees[0].leaves).textureLayer(BlockFace::PosY);
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
    for (const TreeBlocks& wood : m_terrain.blocks().trees) {
        const vxe::u16 leaves = m_view->info(wood.leaves).textureLayer(BlockFace::PosY);
        if (leaves < layerColors.size()) {
            colors.at(leaves) = blend(layerColors[leaves], kFarLeafGain, 0xFF000000U, 0.0);
        }
    }
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

// The ground span of a column with this height and top block.
LodSpan TerrainLodSampler::ground(i64 height, vxe::voxel::BlockStateId top, i64 water) const noexcept {
    LodSpan span{.bottomY = 0, .topY = static_cast<i32>(height)};
    if (height < water) {
        // A lake or the sea: the water surface is what shows.
        span.topY = static_cast<i32>(water);
        span.topMaterial = m_materials.waterOverFloor;
        span.sideMaterial = m_materials.waterOverFloor;
        return span;
    }
    span.topMaterial = m_view->info(top).textureLayer(BlockFace::PosY);
    span.sideMaterial = m_view->info(top).textureLayer(BlockFace::PosX);
    return span;
}

bool TerrainLodSampler::sampleTile(const vxe::voxel::LodTileKey& key,
                                   const vxe::voxel::LodSampleOptions& options,
                                   vxe::voxel::LodTileBuilder& builder, const std::atomic<bool>& cancelled) {
    const i64 cell = key.cellSize();
    constexpr u32 kCells = vxe::voxel::kLodTileCells;
    // Whole terrain columns only for the finest level: a level of 4 block cells
    // would build ten times as many of them for detail the far path has too.
    const bool fine = cell <= 2;
    thread_local std::vector<i64> heights;
    thread_local std::vector<vxe::voxel::BlockStateId> tops;
    thread_local std::vector<f64> covers;
    thread_local std::vector<i64> waters;
    heights.resize(vxe::voxel::kLodCellCount);
    tops.resize(vxe::voxel::kLodCellCount);
    covers.resize(vxe::voxel::kLodCellCount);
    waters.resize(vxe::voxel::kLodCellCount);
    constexpr u32 kGrid = (kCells / kClimateStride) + 1;
    thread_local std::vector<Climate::Field> fields;
    if (!fine) {
        fields.resize(usize{kGrid} * kGrid);
        for (u32 gz = 0; gz < kGrid; ++gz) {
            for (u32 gx = 0; gx < kGrid; ++gx) {
                fields[(usize{gz} * kGrid) + gx] =
                    m_terrain.climate().field(static_cast<f64>(key.originX() + (gx * kClimateStride * cell)),
                                              static_cast<f64>(key.originZ() + (gz * kClimateStride * cell)));
            }
        }
    }
    for (u32 cz = 0; cz < kCells; ++cz) {
        if (cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
        for (u32 cx = 0; cx < kCells; ++cx) {
            const i64 x = key.originX() + (cx * cell) + (cell / 2);
            const i64 z = key.originZ() + (cz * cell) + (cell / 2);
            const u32 index = (cz * kCells) + cx;
            if (fine) {
                heights[index] = m_terrain.surfaceHeight(x, z);
                tops[index] = m_terrain.topBlock(x, z);
                waters[index] = m_terrain.waterLevel(x, z);
                continue;
            }
            const u32 gx = cx / kClimateStride;
            const u32 gz = cz / kClimateStride;
            const f64 tx = (static_cast<f64>(cx % kClimateStride) + 0.5) / kClimateStride;
            const f64 tz = (static_cast<f64>(cz % kClimateStride) + 0.5) / kClimateStride;
            const auto blend = [&](f64 Climate::Field::* part) {
                const Climate::Field* row = &fields[(usize{gz} * kGrid) + gx];
                const f64 near = row[0].*part + ((row[1].*part - row[0].*part) * tx);
                const f64 far = row[kGrid].*part + ((row[kGrid + 1].*part - row[kGrid].*part) * tx);
                return near + ((far - near) * tz);
            };
            const Terrain::FarCell far =
                m_terrain.farCell(static_cast<f64>(x), static_cast<f64>(z), static_cast<f64>(cell),
                                  {.temperature = blend(&Climate::Field::temperature),
                                   .rainfall = blend(&Climate::Field::rainfall),
                                   .shadow = blend(&Climate::Field::shadow),
                                   .variety = blend(&Climate::Field::variety)});
            heights[index] = far.height;
            tops[index] = far.top;
            waters[index] = far.water;
            covers[index] = m_terrain.treeCover(far.biome);
        }
    }
    thread_local Crowns crowns;
    if (fine) {
        placeCrowns(key, crowns);
    }
    std::vector<LodSpan> spans;
    for (u32 cz = 0; cz < kCells; ++cz) {
        for (u32 cx = 0; cx < kCells; ++cx) {
            spans.clear();
            const u32 index = (cz * kCells) + cx;
            const LodSpan base = ground(heights[index], tops[index], waters[index]);
            if (fine) {
                addFineCell(base, crowns, cx, cz, cell, options.maxSpansPerCell, spans);
            } else {
                // A hash of the cell: canopy heights depend on nothing else.
                const i64 bump = ((index * 2654435761U) ^ static_cast<u32>(key.originX())) >> 20U;
                addCoarseCell(base, heights[index], covers[index], bump, spans);
            }
            builder.addCell(spans);
        }
    }
    return true;
}

// Every crown that reaches into the tile, block column by block column: the
// highest top and lowest bottom of leaves over each column, as a round disc.
void TerrainLodSampler::placeCrowns(const vxe::voxel::LodTileKey& key, Crowns& crowns) const {
    const i64 size = key.tileSize();
    const auto columns = static_cast<usize>(size * size);
    crowns.top.assign(columns, kNoCrown);
    crowns.bottom.assign(columns, std::numeric_limits<i32>::max());
    crowns.material.assign(columns, 0);
    constexpr i64 kReach = Terrain::kTreeReach;
    const i64 firstX = floorDiv(key.originX() - kReach, Terrain::kTreeCell);
    const i64 lastX = floorDiv(key.originX() + size + kReach, Terrain::kTreeCell);
    const i64 firstZ = floorDiv(key.originZ() - kReach, Terrain::kTreeCell);
    const i64 lastZ = floorDiv(key.originZ() + size + kReach, Terrain::kTreeCell);
    const TerrainBlocks& blocks = m_terrain.blocks();
    for (i64 cellZ = firstZ; cellZ <= lastZ; ++cellZ) {
        for (i64 cellX = firstX; cellX <= lastX; ++cellX) {
            const auto placed = m_terrain.treeAt(cellX, cellZ);
            if (!placed) {
                continue;
            }
            const Terrain::Tree& tree = placed->tree;
            const auto top = static_cast<i32>(placed->ground + tree.height);
            const auto bottom = static_cast<i32>(placed->ground + tree.height - tree.crownDepth);
            const vxe::u16 material = m_view->info(blocks.trees[static_cast<usize>(tree.kind)].leaves)
                                          .textureLayer(BlockFace::PosY);
            for (i64 dz = -tree.radius; dz <= tree.radius; ++dz) {
                for (i64 dx = -tree.radius; dx <= tree.radius; ++dx) {
                    if ((dx * dx) + (dz * dz) > (tree.radius * tree.radius) + tree.radius) {
                        continue;
                    }
                    const i64 x = tree.crownX + dx - key.originX();
                    const i64 z = tree.crownZ + dz - key.originZ();
                    if (x < 0 || z < 0 || x >= size || z >= size) {
                        continue;
                    }
                    const auto index = static_cast<usize>((z * size) + x);
                    // Domes, not cylinders: the crown falls away from its middle.
                    const i64 reach = (tree.radius * tree.radius) + tree.radius;
                    const auto drop =
                        static_cast<i32>((((dx * dx) + (dz * dz)) * tree.crownDepth * 3) / (reach * 4));
                    if (top - drop > crowns.top[index]) {
                        crowns.top[index] = top - drop;
                        crowns.material[index] = material;
                    }
                    crowns.bottom[index] = std::min(crowns.bottom[index], bottom);
                }
            }
        }
    }
}

void TerrainLodSampler::addFineCell(LodSpan base, const Crowns& crowns, u32 cx, u32 cz, i64 cell,
                                    u32 maxSpans, std::vector<LodSpan>& spans) const {
    const i64 size = cell * vxe::voxel::kLodTileCells;
    i32 crownTop = kNoCrown;
    i32 crownBottom = std::numeric_limits<i32>::max();
    vxe::u16 crownMaterial = m_materials.leaves;
    i64 covered = 0;
    for (i64 z = cz * cell; z < (cz + 1) * cell; ++z) {
        for (i64 x = cx * cell; x < (cx + 1) * cell; ++x) {
            const auto index = static_cast<usize>((z * size) + x);
            if (crowns.top[index] != kNoCrown) {
                ++covered;
                if (crowns.top[index] > crownTop) {
                    crownTop = crowns.top[index];
                    crownMaterial = crowns.material[index];
                }
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
        base.topMaterial = crownMaterial;
        base.sideMaterial = crownMaterial;
        spans.push_back(base);
        return;
    }
    spans.push_back(LodSpan{.bottomY = crownBottom,
                            .topY = crownTop,
                            .topMaterial = crownMaterial,
                            .sideMaterial = crownMaterial});
    spans.push_back(base);
}

void TerrainLodSampler::addCoarseCell(LodSpan base, i64 height, f64 cover, i64 bump,
                                      std::vector<LodSpan>& spans) const {
    if (cover > 0.0 && base.topMaterial == m_materials.grass) {
        const auto eighths = static_cast<u32>(std::lround(cover * kBlendSteps));
        base.topMaterial = grassWithLeaves(eighths);
        if (eighths >= 5) {
            // Forest: from far away the crowns are the ground, in lumps rather
            // than a flat slab.
            base.topY = static_cast<i32>(height + kCrownAboveGround - 3 + (std::abs(bump) % 7));
            base.topMaterial = m_materials.leaves;
            base.sideMaterial = m_materials.leaves;
        }
    }
    spans.push_back(base);
}

} // namespace vr
