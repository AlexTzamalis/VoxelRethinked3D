#include "terrain.hpp"

#include <vxe/core/error.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/math/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>
#include <vector>

namespace vr {

using vxe::f32;
using vxe::f64;
using vxe::i32;
using vxe::i64;
using vxe::u32;
using vxe::u64;
using vxe::u8;
using vxe::usize;
using vxe::noise::smooth;
using vxe::noise::unit;
using vxe::voxel::BlockStateId;

namespace {

constexpr i64 kGridStep = 4;         // Blocks between the heights a column blends.
constexpr usize kGridPoints = 5;     // Per side of a chunk, both edges included.
constexpr f64 kSlopeReach = 6.0;     // Blocks over which the planet's slope is measured.
constexpr f64 kGullyWidest = 180.0;  // Blocks across the widest gullies.
constexpr f64 kGullyStrength = 0.15; // Depth of a gully, as a part of its width.
constexpr int kGullyOctaves = 3;
constexpr f64 kRockSlope = 1.25;     // Steeper ground than this holds no soil.
constexpr f64 kRockLine = 470.0;     // Blocks above the sea where soil ends.
constexpr f64 kSnowLine = 720.0;     // Blocks above the sea where snow stays.
constexpr f64 kLineWander = 46.0;    // Blocks the two lines wander, so they are no contours.
constexpr f64 kOverhangMost = 14.0;  // Blocks a rock face leans at most.
constexpr i64 kTreeHeadroom = 24;    // Trees on higher neighbors can reach this far above a column.
constexpr usize kCacheColumns = 256; // Per thread, a power of two.

constexpr u64 mix(u64 value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

constexpr u64 hash3(u64 seed, i64 x, i64 y, i64 z) noexcept {
    return mix(seed ^ mix(static_cast<u64>(x) ^ mix(static_cast<u64>(y) ^ mix(static_cast<u64>(z)))));
}

i64 floorDiv(i64 value, i64 divisor) noexcept {
    return value >= 0 ? value / divisor : ((value + 1) / divisor) - 1;
}

// A smooth wave with a period of 1, 1 at whole numbers and -1 half way, and
// its slope. A stand in for a cosine: library cosines differ between machines,
// and worlds must not (see vxe/math/noise.hpp).
struct Wave {
    f64 value = 0.0;
    f64 slope = 0.0;
};
Wave wave(f64 phase) noexcept {
    const f64 t = phase - std::floor(phase);
    const f64 side = t < 0.5 ? 1.0 : -1.0;
    const f64 s = t < 0.5 ? 1.0 - (2.0 * t) : (2.0 * t) - 1.0; // 1 at whole numbers, 0 half way.
    return {.value = (2.0 * s * s * (3.0 - (2.0 * s))) - 1.0, .slope = -24.0 * s * (1.0 - s) * side};
}

} // namespace

// The heights of one chunk column, indexed by z * 16 + x.
struct Terrain::Column {
    u64 identity = 0;
    i32 chunkX = 0;
    i32 chunkZ = 0;
    bool valid = false;
    i32 lowest = 0;
    i32 highest = 0;
    f32 leanMost = 0.0F; // The highest of `lean`.
    std::array<i32, vxe::kChunkArea> heights{};
    std::array<f32, vxe::kChunkArea> lean{}; // Blocks the rock face leans here.
    std::array<Surface, vxe::kChunkArea> surface{};
};

vxe::Result<TerrainBlocks> TerrainBlocks::fromCatalog(const BlockCatalog& catalog) {
    TerrainBlocks blocks;
    const std::array<std::pair<std::string_view, BlockStateId*>, 13> wanted = {{
        {"stone", &blocks.stone},
        {"dirt", &blocks.dirt},
        {"grass_block", &blocks.grass},
        {"sand", &blocks.sand},
        {"gravel", &blocks.gravel},
        {"water", &blocks.water},
        {"snow_block", &blocks.snow},
        {"coal_ore", &blocks.coal},
        {"iron_ore", &blocks.iron},
        {"gold_ore", &blocks.gold},
        {"foundation_stone", &blocks.floor},
        {"oak_log[axis=y]", &blocks.log},
        {"oak_leaves", &blocks.leaves},
    }};
    for (const auto& [name, slot] : wanted) {
        const auto state = catalog.stateId(name);
        if (!state) {
            return vxe::Error(vxe::ErrorCode::NotFound,
                              std::format("The terrain needs the block '{}'", name));
        }
        *slot = *state;
    }
    return blocks;
}

Terrain::Terrain(const TerrainBlocks& blocks, u64 seed, const PlanetPreset& preset) noexcept
    : m_blocks(blocks)
    , m_seed(seed)
    , m_identity(mix(seed) ^ vxe::hashBytes(std::as_bytes(std::span(&preset, 1))))
    , m_planet(seed, preset) {}

void Terrain::gullies(f64 x, f64 z, f64 shortest, f64 strength, Ground& out, f64& slopeX,
                      f64& slopeZ) const noexcept {
    f64 widest = kGullyWidest;
    u64 seed = m_seed + 31;
    for (int octave = 0; octave < kGullyOctaves && widest >= shortest; ++octave) {
        const f64 steepness = std::sqrt((slopeX * slopeX) + (slopeZ * slopeZ));
        if (steepness < 1.0e-6) {
            return;
        }
        // Across the slope: moving this way goes neither up nor down, so
        // stripes that change along it run downhill.
        const f64 acrossX = -slopeZ / steepness;
        const f64 acrossZ = slopeX / steepness;
        const f64 px = x / widest;
        const f64 pz = z / widest;
        const auto cellX = static_cast<i64>(std::floor(px));
        const auto cellZ = static_cast<i64>(std::floor(pz));
        // Stripes come from nearby random spots, each laying its own stripes
        // through itself, blended by distance. One global set of stripes
        // would bend with the slope into rings.
        f64 weights = 0.0;
        f64 value = 0.0;
        f64 change = 0.0;
        for (i64 dz = -1; dz <= 1; ++dz) {
            for (i64 dx = -1; dx <= 1; ++dx) {
                const u64 h = vxe::noise::hash2(seed, cellX + dx, cellZ + dz);
                const f64 gapX = px - (static_cast<f64>(cellX + dx) + unit(h));
                const f64 gapZ = pz - (static_cast<f64>(cellZ + dz) + unit(mix(h)));
                const f64 near = 1.0 - (((gapX * gapX) + (gapZ * gapZ)) / 2.25);
                if (near <= 0.0) {
                    continue;
                }
                const f64 weight = near * near * near;
                const Wave stripes = wave((gapX * acrossX) + (gapZ * acrossZ));
                weights += weight;
                value += stripes.value * weight;
                change += stripes.slope * weight;
            }
        }
        if (weights <= 0.0) {
            return;
        }
        // More channel than ridge: erosion takes ground away.
        const f64 depth = widest * kGullyStrength * strength;
        out.height += ((value / weights) - 0.3) * depth;
        // The bend of this octave, for the next to follow.
        const f64 bend = (change / weights) * depth / widest;
        slopeX += bend * acrossX;
        slopeZ += bend * acrossZ;
        widest *= 0.5;
        seed = mix(seed);
    }
}

Ground Terrain::ground(f64 x, f64 z, f64 shortest) const noexcept {
    const f64 here = m_planet.sample(x, z).elevation;
    f64 slopeX = (m_planet.sample(x + kSlopeReach, z).elevation - here) / kSlopeReach;
    f64 slopeZ = (m_planet.sample(x, z + kSlopeReach).elevation - here) / kSlopeReach;
    Ground out{.height = here + static_cast<f64>(kSeaLevel),
               .slope = std::sqrt((slopeX * slopeX) + (slopeZ * slopeZ))};
    // Water carves what stands above the sea and is steep enough to run.
    const f64 strength = smooth(0.06, 0.55, out.slope) * smooth(2.0, 50.0, here);
    if (strength > 0.0) {
        gullies(x, z, shortest, strength, out, slopeX, slopeZ);
        out.slope = std::sqrt((slopeX * slopeX) + (slopeZ * slopeZ));
    }
    return out;
}

Ground Terrain::farGround(f64 x, f64 z, f64 cell) const noexcept {
    if (cell >= kGullyWidest) {
        // Wider than any gully, and too wide for a slope to mean much.
        return {.height = m_planet.sample(x, z).elevation + static_cast<f64>(kSeaLevel), .slope = 0.0};
    }
    return ground(x, z, cell * 2.0);
}

Surface Terrain::surfaceOf(f64 x, f64 z, const Ground& at) const noexcept {
    const f64 above = at.height - static_cast<f64>(kSeaLevel);
    if (above <= 1.0) {
        return Surface::Sand;
    }
    const f64 wander = vxe::noise::gradient2(m_seed + 41, x / 37.0, z / 37.0).value * kLineWander;
    if (above + wander > kSnowLine && at.slope < kRockSlope * 1.3) {
        return Surface::Snow;
    }
    return at.slope > kRockSlope || above + wander > kRockLine ? Surface::Rock : Surface::Grass;
}

void Terrain::buildColumn(i32 chunkX, i32 chunkZ, Column& column) const noexcept {
    const vxe::BlockPos origin = vxe::chunkOrigin({.x = chunkX, .y = 0, .z = chunkZ});
    std::array<Ground, kGridPoints * kGridPoints> grid;
    for (usize gz = 0; gz < kGridPoints; ++gz) {
        for (usize gx = 0; gx < kGridPoints; ++gx) {
            grid[(gz * kGridPoints) + gx] =
                ground(static_cast<f64>(origin.x + (static_cast<i64>(gx) * kGridStep)),
                       static_cast<f64>(origin.z + (static_cast<i64>(gz) * kGridStep)), 1.0);
        }
    }
    column.identity = m_identity;
    column.chunkX = chunkX;
    column.chunkZ = chunkZ;
    column.valid = true;
    column.lowest = 1 << 30;
    column.highest = -(1 << 30);
    column.leanMost = 0.0F;
    for (i64 z = 0; z < vxe::kChunkSize; ++z) {
        for (i64 x = 0; x < vxe::kChunkSize; ++x) {
            const auto gx = static_cast<usize>(x / kGridStep);
            const auto gz = static_cast<usize>(z / kGridStep);
            const f64 tx = static_cast<f64>(x % kGridStep) / static_cast<f64>(kGridStep);
            const f64 tz = static_cast<f64>(z % kGridStep) / static_cast<f64>(kGridStep);
            const auto blend = [&](f64 Ground::* part) {
                const Ground* row = &grid[(gz * kGridPoints) + gx];
                const f64 near = row[0].*part + ((row[1].*part - row[0].*part) * tx);
                const f64 far =
                    row[kGridPoints].*part + ((row[kGridPoints + 1].*part - row[kGridPoints].*part) * tx);
                return near + ((far - near) * tz);
            };
            const auto wx = static_cast<f64>(origin.x + x);
            const auto wz = static_cast<f64>(origin.z + z);
            Ground here{.height = blend(&Ground::height), .slope = blend(&Ground::slope)};
            // The grid is 4 blocks wide; this gives the ground its last grain.
            const f64 above = here.height - static_cast<f64>(kSeaLevel);
            here.height +=
                vxe::noise::gradient2(m_seed + 37, wx / 9.0, wz / 9.0).value * smooth(1.0, 8.0, above) * 1.6;
            const auto height = static_cast<i32>(std::floor(here.height));
            const auto index = static_cast<usize>((z * vxe::kChunkSize) + x);
            column.heights[index] = height;
            column.surface[index] = surfaceOf(wx, wz, here);
            const auto lean =
                static_cast<f32>(kOverhangMost * smooth(1.1, 2.6, here.slope) * smooth(30.0, 140.0, above));
            column.lean[index] = lean;
            column.leanMost = std::max(column.leanMost, lean);
            column.lowest = std::min(column.lowest, height);
            column.highest = std::max(column.highest, height);
        }
    }
}

const Terrain::Column& Terrain::column(i32 chunkX, i32 chunkZ) const {
    // A plain direct mapped table: no locks, because each thread has its own,
    // and no eviction logic, because a wrong guess only costs a rebuild.
    thread_local std::vector<Column> cache(kCacheColumns);
    const u64 h =
        mix(m_identity ^ ((static_cast<u64>(static_cast<u32>(chunkX)) << 32U) | static_cast<u32>(chunkZ)));
    Column& entry = cache[h & (kCacheColumns - 1)];
    if (!entry.valid || entry.identity != m_identity || entry.chunkX != chunkX || entry.chunkZ != chunkZ) {
        buildColumn(chunkX, chunkZ, entry);
    }
    return entry;
}

vxe::i64 Terrain::surfaceHeight(i64 x, i64 z) const {
    const Column& at = column(static_cast<i32>(floorDiv(x, vxe::kChunkSize)),
                              static_cast<i32>(floorDiv(z, vxe::kChunkSize)));
    return at.heights[static_cast<usize>(((z & 15) * vxe::kChunkSize) + (x & 15))];
}

Surface Terrain::surfaceAt(i64 x, i64 z) const {
    const Column& at = column(static_cast<i32>(floorDiv(x, vxe::kChunkSize)),
                              static_cast<i32>(floorDiv(z, vxe::kChunkSize)));
    return at.surface[static_cast<usize>(((z & 15) * vxe::kChunkSize) + (x & 15))];
}

vxe::BlockPos Terrain::spawnColumn() const {
    // Rings around the origin, each 96 blocks further out.
    constexpr i64 kRingStep = 96;
    constexpr i64 kRings = 300;
    for (i64 ring = 0; ring < kRings; ++ring) {
        const i64 reach = ring * kRingStep;
        const i64 count = ring == 0 ? 1 : ring * 8;
        for (i64 i = 0; i < count; ++i) {
            // The square ring, side by side.
            const i64 side = ring == 0 ? 0 : i / (ring * 2);
            const i64 along = ring == 0 ? 0 : ((i % (ring * 2)) - ring) * kRingStep;
            const std::array<std::pair<i64, i64>, 4> corners = {
                {{along, -reach}, {reach, along}, {-along, reach}, {-reach, -along}}};
            const auto [x, z] = corners.at(static_cast<usize>(side));
            const Ground at = ground(static_cast<f64>(x), static_cast<f64>(z), kGullyWidest);
            const f64 above = at.height - static_cast<f64>(kSeaLevel);
            if (above > 6.0 && above < 70.0 && at.slope < 0.3) {
                return {.x = x, .y = surfaceHeight(x, z), .z = z};
            }
        }
    }
    return {.x = 0, .y = surfaceHeight(0, 0), .z = 0};
}

BlockStateId Terrain::stoneAt(i64 x, i64 y, i64 z) const noexcept {
    // Ores by depth, then gravel pockets where slow 3D noise is high.
    const f64 roll = unit(hash3(m_seed + 3, x, y, z));
    if (roll < 0.012) {
        return m_blocks.coal;
    }
    if (roll < 0.018 && y < 48) {
        return m_blocks.iron;
    }
    if (roll < 0.0205 && y < 24) {
        return m_blocks.gold;
    }
    const f64 pocket = vxe::noise::gradient3(m_seed + 5, static_cast<f64>(x) / 9.0, static_cast<f64>(y) / 9.0,
                                             static_cast<f64>(z) / 9.0);
    return pocket > 0.42 ? m_blocks.gravel : m_blocks.stone;
}

void Terrain::generate(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const Column& at = column(chunk.x, chunk.z);
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const auto leanReach = static_cast<i64>(std::ceil(at.leanMost));
    // Chunks wholly above ground and water are air: skip the work (Section 12.2).
    if (origin.y > std::max<i64>(at.highest + leanReach, kSeaLevel) + kTreeHeadroom) {
        std::ranges::fill(out, vxe::voxel::kAirState);
        return;
    }
    // Faces only lean where this chunk meets them. Density is costly, so it
    // comes on a grid of 4 x 8 x 4 blocks (Section 12.2), blended between.
    const bool leaning = leanReach > 0 && origin.y <= at.highest + leanReach &&
                         origin.y + vxe::kChunkSize > at.lowest - leanReach;
    std::array<f64, usize{5} * 3 * 5> density{};
    if (leaning) {
        const vxe::noise::Fractal shape{.octaves = 3, .frequency = 1.0 / 26.0};
        for (usize y = 0; y < 3; ++y) {
            for (usize z = 0; z < 5; ++z) {
                for (usize x = 0; x < 5; ++x) {
                    density[(y * 25) + (z * 5) + x] =
                        vxe::noise::fbm3(m_seed + 43,
                                         static_cast<f64>(origin.x) + (static_cast<f64>(x) * 4.0),
                                         static_cast<f64>(origin.y) + (static_cast<f64>(y) * 8.0),
                                         static_cast<f64>(origin.z) + (static_cast<f64>(z) * 4.0), shape) *
                        1.8;
                }
            }
        }
    }
    const auto densityAt = [&](u32 x, u32 y, u32 z) {
        const u32 gx = x / 4;
        const u32 gy = y / 8;
        const u32 gz = z / 4;
        const f64 tx = static_cast<f64>(x % 4) / 4.0;
        const f64 ty = static_cast<f64>(y % 8) / 8.0;
        const f64 tz = static_cast<f64>(z % 4) / 4.0;
        const auto corner = [&](u32 dx, u32 dy, u32 dz) {
            return density[(static_cast<usize>(gy + dy) * 25) + (static_cast<usize>(gz + dz) * 5) +
                           static_cast<usize>(gx + dx)];
        };
        const auto layer = [&](u32 dy) {
            const f64 near = corner(0, dy, 0) + ((corner(1, dy, 0) - corner(0, dy, 0)) * tx);
            const f64 far = corner(0, dy, 1) + ((corner(1, dy, 1) - corner(0, dy, 1)) * tx);
            return near + ((far - near) * tz);
        };
        return layer(0) + ((layer(1) - layer(0)) * ty);
    };
    for (u32 index = 0; index < static_cast<u32>(vxe::kChunkVolume); ++index) {
        const vxe::LocalPos local = vxe::LocalPos::fromIndex(static_cast<vxe::u16>(index));
        const usize columnIndex = (usize{local.z} * vxe::kChunkSize) + local.x;
        const i64 x = origin.x + local.x;
        const i64 y = origin.y + local.y;
        const i64 z = origin.z + local.z;
        if (y <= kFloorY) {
            out[index] = m_blocks.floor;
            continue;
        }
        f64 top = at.heights[columnIndex];
        if (leaning && at.lean[columnIndex] > 0.0F) {
            top -= static_cast<f64>(at.lean[columnIndex]) * densityAt(local.x, local.y, local.z);
        }
        const auto height = static_cast<f64>(y);
        if (height > top) {
            out[index] = y <= kSeaLevel ? m_blocks.water : vxe::voxel::kAirState;
            continue;
        }
        const f64 depth = top - height;
        // Caves: winding tunnels where 3D noise is near zero, kept away from
        // the surface so lakes do not drain into them.
        if (depth > 6.0) {
            const f64 cave = vxe::noise::gradient3(m_seed + 11, static_cast<f64>(x) / 18.0,
                                                   static_cast<f64>(y) / 12.0, static_cast<f64>(z) / 18.0);
            if (std::abs(cave) < 0.05) {
                out[index] = vxe::voxel::kAirState;
                continue;
            }
        }
        const Surface surface = at.surface[columnIndex];
        if (depth < 1.0) {
            switch (surface) {
            case Surface::Grass:
                out[index] = m_blocks.grass;
                break;
            case Surface::Sand:
                out[index] = m_blocks.sand;
                break;
            case Surface::Snow:
                out[index] = m_blocks.snow;
                break;
            case Surface::Rock:
                out[index] = m_blocks.stone;
                break;
            }
            continue;
        }
        if (depth < 4.0 && (surface == Surface::Grass || surface == Surface::Sand)) {
            out[index] = surface == Surface::Grass ? m_blocks.dirt : m_blocks.sand;
            continue;
        }
        out[index] = stoneAt(x, y, z);
    }
    placeTrees(chunk, out);
}

void Terrain::placeTrees(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    // Every cell whose tree could reach into this chunk (leaves spread 2).
    const i64 firstCellX = floorDiv(origin.x - kLeafRadius, kTreeCell);
    const i64 lastCellX = floorDiv(origin.x + vxe::kChunkSize + kLeafRadius, kTreeCell);
    const i64 firstCellZ = floorDiv(origin.z - kLeafRadius, kTreeCell);
    const i64 lastCellZ = floorDiv(origin.z + vxe::kChunkSize + kLeafRadius, kTreeCell);
    for (i64 cellZ = firstCellZ; cellZ <= lastCellZ; ++cellZ) {
        for (i64 cellX = firstCellX; cellX <= lastCellX; ++cellX) {
            stampTree(origin, cellX, cellZ, out);
        }
    }
}

std::optional<Terrain::TreeSpot> Terrain::treeSpot(i64 cellX, i64 cellZ) const noexcept {
    const u64 h = hash3(m_seed + 17, cellX, 0, cellZ);
    if (unit(h) > 0.45) {
        return std::nullopt; // No tree in this cell.
    }
    return TreeSpot{.x = (cellX * kTreeCell) + 1 + static_cast<i64>((h >> 8U) % 5),
                    .z = (cellZ * kTreeCell) + 1 + static_cast<i64>((h >> 16U) % 5),
                    .trunk = 4 + static_cast<i64>((h >> 24U) % 3)};
}

void Terrain::stampTree(const vxe::BlockPos& origin, i64 cellX, i64 cellZ,
                        std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const std::optional<TreeSpot> spot = treeSpot(cellX, cellZ);
    if (!spot) {
        return;
    }
    const i64 x = spot->x;
    const i64 z = spot->z;
    const i64 ground = surfaceHeight(x, z);
    // A tree far above or below this chunk has nothing to stamp here.
    if (ground + spot->trunk + 1 < origin.y || ground > origin.y + vxe::kChunkSize) {
        return;
    }
    if (!treeGrows(surfaceAt(x, z))) {
        return;
    }
    // Writes one block if it falls inside this chunk. Leaves only fill air;
    // the trunk replaces whatever is there.
    const auto put = [&](i64 bx, i64 by, i64 bz, BlockStateId state, bool replaceSolid) {
        const i64 lx = bx - origin.x;
        const i64 ly = by - origin.y;
        const i64 lz = bz - origin.z;
        if (lx < 0 || ly < 0 || lz < 0 || lx >= vxe::kChunkSize || ly >= vxe::kChunkSize ||
            lz >= vxe::kChunkSize) {
            return;
        }
        const vxe::u16 index =
            vxe::LocalPos{.x = static_cast<u8>(lx), .y = static_cast<u8>(ly), .z = static_cast<u8>(lz)}
                .index();
        if (replaceSolid || out[index] == vxe::voxel::kAirState) {
            out[index] = state;
        }
    };
    const i64 top = ground + spot->trunk;
    for (i64 y = top - 2; y <= top + 1; ++y) {
        const i64 radius = y >= top ? 1 : kLeafRadius;
        for (i64 dz = -radius; dz <= radius; ++dz) {
            for (i64 dx = -radius; dx <= radius; ++dx) {
                // Trim the corners of the wide layers so crowns look round.
                if (radius != kLeafRadius || std::abs(dx) != radius || std::abs(dz) != radius) {
                    put(x + dx, y, z + dz, m_blocks.leaves, false);
                }
            }
        }
    }
    for (i64 y = ground + 1; y < top; ++y) {
        put(x, y, z, m_blocks.log, true);
    }
}

} // namespace vr
