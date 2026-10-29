#include "demo_terrain.hpp"

#include <vxe/core/error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>

namespace vr {

using vxe::f64;
using vxe::i32;
using vxe::i64;
using vxe::u32;
using vxe::u64;
using vxe::usize;
using vxe::voxel::BlockStateId;

namespace {

// SplitMix64 finalizer (Section 12.3): turns coordinates and a seed into
// well mixed bits, so neighboring positions get unrelated values.
constexpr u64 mix(u64 value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

constexpr u64 hash3(u64 seed, i64 x, i64 y, i64 z) noexcept {
    return mix(seed ^ mix(static_cast<u64>(x) ^ mix(static_cast<u64>(y) ^ mix(static_cast<u64>(z)))));
}

// A hash as a number from 0 to 1.
constexpr f64 unit(u64 hash) noexcept {
    return static_cast<f64>(hash >> 11U) * (1.0 / 9007199254740992.0);
}

constexpr f64 smooth(f64 t) noexcept {
    return t * t * (3.0 - (2.0 * t));
}

// Value noise: random values on an integer grid, blended smoothly between
// them. Coordinates are doubles, so precision holds far from the origin
// (Section 12.4).
f64 noise2(u64 seed, f64 x, f64 z) noexcept {
    const f64 fx = std::floor(x);
    const f64 fz = std::floor(z);
    const auto ix = static_cast<i64>(fx);
    const auto iz = static_cast<i64>(fz);
    const f64 tx = smooth(x - fx);
    const f64 tz = smooth(z - fz);
    const auto at = [&](i64 dx, i64 dz) { return unit(hash3(seed, ix + dx, 0, iz + dz)); };
    const f64 near = at(0, 0) + ((at(1, 0) - at(0, 0)) * tx);
    const f64 far = at(0, 1) + ((at(1, 1) - at(0, 1)) * tx);
    return near + ((far - near) * tz);
}

f64 noise3(u64 seed, f64 x, f64 y, f64 z) noexcept {
    const f64 fy = std::floor(y);
    const f64 ty = smooth(y - fy);
    const auto iy = static_cast<i64>(fy);
    const f64 below = noise2(seed ^ mix(static_cast<u64>(iy)), x, z);
    const f64 above = noise2(seed ^ mix(static_cast<u64>(iy + 1)), x, z);
    return below + ((above - below) * ty);
}

// Octaves of noise, each twice as fine and half as strong. Returns 0 to 1.
f64 fractal2(u64 seed, f64 x, f64 z, i32 octaves) noexcept {
    f64 sum = 0.0;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    for (i32 octave = 0; octave < octaves; ++octave) {
        sum += noise2(seed + static_cast<u64>(octave), x, z) * amplitude;
        total += amplitude;
        x *= 2.0;
        z *= 2.0;
        amplitude *= 0.5;
    }
    return sum / total;
}

} // namespace

vxe::Result<DemoBlocks> DemoBlocks::fromCatalog(const BlockCatalog& catalog) {
    DemoBlocks blocks;
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
                              std::format("The demo terrain needs the block '{}'", name));
        }
        *slot = *state;
    }
    return blocks;
}

i64 DemoTerrain::surfaceHeight(i64 x, i64 z) const noexcept {
    const auto fx = static_cast<f64>(x);
    const auto fz = static_cast<f64>(z);
    // Large slow shapes decide lowland or highland; hills ride on top, and
    // the highlands get sharper, taller relief.
    const f64 continent = fractal2(m_seed, fx / 220.0, fz / 220.0, 3);
    const f64 hills = fractal2(m_seed + 100, fx / 48.0, fz / 48.0, 4);
    const f64 highland = std::clamp((continent - 0.45) * 3.0, 0.0, 1.0);
    const f64 height = 52.0 + (continent * 24.0) + ((hills - 0.5) * (10.0 + (highland * 40.0)));
    return std::clamp(static_cast<i64>(std::floor(height)), i64{8}, i64{124});
}

BlockStateId DemoTerrain::stoneAt(i64 x, i64 y, i64 z) const noexcept {
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
    const f64 pocket =
        noise3(m_seed + 5, static_cast<f64>(x) / 9.0, static_cast<f64>(y) / 9.0, static_cast<f64>(z) / 9.0);
    return pocket > 0.78 ? m_blocks.gravel : m_blocks.stone;
}

BlockStateId DemoTerrain::blockAt(i64 x, i64 y, i64 z, i64 height) const noexcept {
    if (y <= 0 || (y <= 2 && unit(hash3(m_seed + 7, x, y, z)) < 0.5)) {
        return m_blocks.floor;
    }
    if (y > height) {
        return y <= kSeaLevel ? m_blocks.water : vxe::voxel::kAirState;
    }
    // Caves: winding tunnels where 3D noise is near its middle, kept away
    // from the surface so lakes do not drain into them.
    if (y > 3 && y < height - 5) {
        const f64 cave = noise3(m_seed + 11, static_cast<f64>(x) / 18.0, static_cast<f64>(y) / 12.0,
                                static_cast<f64>(z) / 18.0);
        if (std::abs(cave - 0.5) < 0.035) {
            return vxe::voxel::kAirState;
        }
    }
    const bool beach = height <= kSeaLevel + 1;
    if (y == height) {
        if (beach) {
            return m_blocks.sand;
        }
        return height > 100 ? m_blocks.snow : m_blocks.grass;
    }
    if (y > height - 4) {
        return beach ? m_blocks.sand : m_blocks.dirt;
    }
    return stoneAt(x, y, z);
}

void DemoTerrain::generate(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    std::array<i64, vxe::kChunkArea> heights{};
    i64 highest = 0;
    for (usize column = 0; column < heights.size(); ++column) {
        const auto x = static_cast<i64>(column % vxe::kChunkSize);
        const auto z = static_cast<i64>(column / vxe::kChunkSize);
        heights.at(column) = surfaceHeight(origin.x + x, origin.z + z);
        highest = std::max(highest, heights.at(column));
    }
    // Chunks wholly above ground and water are air: skip the work (Section 12.2).
    if (origin.y > std::max(highest, kSeaLevel) + 8) {
        std::ranges::fill(out, vxe::voxel::kAirState);
        return;
    }
    for (u32 index = 0; index < static_cast<u32>(vxe::kChunkVolume); ++index) {
        const vxe::LocalPos local = vxe::LocalPos::fromIndex(static_cast<vxe::u16>(index));
        const i64 height = heights.at((usize{local.z} * vxe::kChunkSize) + local.x);
        out[index] = blockAt(origin.x + local.x, origin.y + local.y, origin.z + local.z, height);
    }
    placeTrees(chunk, out);
}

void DemoTerrain::placeTrees(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const auto floorDiv = [](i64 value, i64 divisor) {
        return value >= 0 ? value / divisor : ((value + 1) / divisor) - 1;
    };
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

std::optional<DemoTerrain::TreeSpot> DemoTerrain::treeSpot(i64 cellX, i64 cellZ) const noexcept {
    const u64 h = hash3(m_seed + 17, cellX, 0, cellZ);
    if (unit(h) > 0.45) {
        return std::nullopt; // No tree in this cell.
    }
    return TreeSpot{.x = (cellX * kTreeCell) + 1 + static_cast<i64>((h >> 8U) % 5),
                    .z = (cellZ * kTreeCell) + 1 + static_cast<i64>((h >> 16U) % 5),
                    .trunk = 4 + static_cast<i64>((h >> 24U) % 3)};
}

void DemoTerrain::stampTree(const vxe::BlockPos& origin, i64 cellX, i64 cellZ,
                            std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const std::optional<TreeSpot> spot = treeSpot(cellX, cellZ);
    if (!spot) {
        return;
    }
    const i64 x = spot->x;
    const i64 z = spot->z;
    const i64 ground = surfaceHeight(x, z);
    if (!treeGrows(ground)) {
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
        const vxe::u16 index = vxe::LocalPos{.x = static_cast<vxe::u8>(lx),
                                             .y = static_cast<vxe::u8>(ly),
                                             .z = static_cast<vxe::u8>(lz)}
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
