#pragma once

// A synthetic terrain sample for chunk storage tests and benchmarks: rolling
// hills with grass, dirt and stone layers, sand at the shore, water below sea
// level, scattered ores and caves. It stands in for real world generation
// (G3) when measuring memory and speed against the Section 2 targets.

#include <vxe/math/coords.hpp>
#include <vxe/voxel/chunk_storage.hpp>

#include <array>
#include <cmath>
#include <span>

namespace vxe::test {

namespace terrain {
inline constexpr voxel::BlockStateId kAir = 0;
inline constexpr voxel::BlockStateId kStone = 1;
inline constexpr voxel::BlockStateId kDirt = 2;
inline constexpr voxel::BlockStateId kGrass = 3;
inline constexpr voxel::BlockStateId kCoal = 4;
inline constexpr voxel::BlockStateId kIron = 5;
inline constexpr voxel::BlockStateId kGold = 6;
inline constexpr voxel::BlockStateId kWater = 7;
inline constexpr voxel::BlockStateId kSand = 8;
inline constexpr voxel::BlockStateId kGravel = 9;
inline constexpr i64 kSeaLevel = 62;
} // namespace terrain

// A deterministic hash of a block position to [0, 1).
inline double hashUnit(i64 x, i64 y, i64 z) {
    u64 h = static_cast<u64>(x) * 0x9E3779B97F4A7C15ULL ^ static_cast<u64>(y) * 0xC2B2AE3D27D4EB4FULL ^
            static_cast<u64>(z) * 0x165667B19E3779F9ULL;
    h ^= h >> 29U;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 32U;
    return static_cast<double>(h >> 11U) * (1.0 / 9007199254740992.0);
}

inline voxel::BlockStateId sampleTerrain(i64 x, i64 y, i64 z) {
    using namespace terrain;
    const double fx = static_cast<double>(x);
    const double fz = static_cast<double>(z);
    const auto height = static_cast<i64>(std::floor(
        64.0 + 10.0 * std::sin(fx * 0.045) + 7.0 * std::cos(fz * 0.06) + 4.0 * std::sin((fx + fz) * 0.11)));
    const double fy = static_cast<double>(y);
    // Caves: where three slow waves line up, below the surface.
    const bool cave =
        y < height - 4 && std::sin(fx * 0.12) * std::sin(fy * 0.15) * std::sin(fz * 0.13) > 0.35;
    if (y > height) {
        return y <= kSeaLevel ? kWater : kAir;
    }
    if (cave) {
        return kAir;
    }
    if (y == height) {
        return height <= kSeaLevel + 1 ? kSand : kGrass;
    }
    if (y > height - 4) {
        return height <= kSeaLevel + 1 ? kSand : kDirt;
    }
    const double roll = hashUnit(x, y, z);
    if (roll < 0.010) {
        return kCoal;
    }
    if (roll < 0.015 && y < 40) {
        return kIron;
    }
    if (roll < 0.017 && y < 16) {
        return kGold;
    }
    if (roll < 0.022) {
        return kGravel;
    }
    return kStone;
}

// The 4096 states of one chunk of the sample, in block order.
inline void sampleChunk(ChunkPos chunk, std::span<voxel::BlockStateId, kChunkVolume> out) {
    const BlockPos origin = chunkOrigin(chunk);
    for (u16 i = 0; i < kChunkVolume; ++i) {
        const LocalPos local = LocalPos::fromIndex(i);
        out[i] = sampleTerrain(origin.x + local.x, origin.y + local.y, origin.z + local.z);
    }
}

} // namespace vxe::test
