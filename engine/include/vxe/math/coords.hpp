#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <format>
#include <functional>

// Coordinate spaces of the voxel world (Section 9.1 of the plan).
//
// Strong types keep the spaces apart, because mixing them up is one of the
// most common bugs in voxel engines:
//   ChunkPos  Position of a whole chunk in the chunk grid. One unit is one
//             chunk (16 blocks). int32 gives about plus or minus 34 billion
//             blocks on every axis.
//   LocalPos  Position of a block inside its chunk, each component 0 to 15.
//   BlockPos  Position of a block in the world. int64, so chunk * 16 + local
//             can never overflow, even at the very edge of the chunk grid.
// Entities use DVec3 (double) positions; blockAt() finds the block they are in.
namespace vxe {

inline constexpr i32 kChunkSize = 16;
inline constexpr i32 kChunkShift = 4; // 16 == 1 << 4
inline constexpr i32 kChunkMask = kChunkSize - 1;
inline constexpr i32 kChunkArea = kChunkSize * kChunkSize;
inline constexpr i32 kChunkVolume = kChunkSize * kChunkSize * kChunkSize;

struct ChunkPos {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    friend constexpr auto operator<=>(const ChunkPos&, const ChunkPos&) = default;

    // Moves by a whole number of chunks.
    [[nodiscard]] constexpr ChunkPos offset(i32 dx, i32 dy, i32 dz) const noexcept {
        return {x + dx, y + dy, z + dz};
    }
    // The IVec3 functions are not constexpr: GLM_FORCE_INTRINSICS makes glm
    // vectors SIMD types, whose constructors cannot run at compile time.
    [[nodiscard]] ChunkPos operator+(const IVec3& delta) const noexcept {
        return offset(delta.x, delta.y, delta.z);
    }
    [[nodiscard]] IVec3 operator-(const ChunkPos& other) const noexcept {
        return {x - other.x, y - other.y, z - other.z};
    }
    [[nodiscard]] IVec3 toIVec3() const noexcept { return {x, y, z}; }
};

struct LocalPos {
    u8 x = 0;
    u8 y = 0;
    u8 z = 0;

    friend constexpr auto operator<=>(const LocalPos&, const LocalPos&) = default;

    // Index into the 4096 blocks of a chunk. The order is y * 256 + z * 16 + x
    // (Y major, Section 9.4), so one horizontal slice of a chunk is one
    // contiguous run of 256 blocks, which helps meshing and light propagation.
    [[nodiscard]] constexpr u16 index() const noexcept {
        VXE_ASSERT(x < kChunkSize && y < kChunkSize && z < kChunkSize, "LocalPos out of range");
        return static_cast<u16>((y << 8U) | (z << 4U) | x);
    }

    // The inverse of index().
    [[nodiscard]] static constexpr LocalPos fromIndex(u16 index) noexcept {
        VXE_ASSERT(index < kChunkVolume, "Block index out of range");
        return {static_cast<u8>(index & 15U), static_cast<u8>(index >> 8U),
                static_cast<u8>((index >> 4U) & 15U)};
    }
};

struct BlockPos {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;

    friend constexpr auto operator<=>(const BlockPos&, const BlockPos&) = default;

    [[nodiscard]] constexpr BlockPos offset(i64 dx, i64 dy, i64 dz) const noexcept {
        return {x + dx, y + dy, z + dz};
    }
    [[nodiscard]] BlockPos operator+(const IVec3& delta) const noexcept {
        return offset(delta.x, delta.y, delta.z);
    }
};

// The chunk that contains a block. An arithmetic right shift rounds towards
// negative infinity, which is exactly what is needed: block -1 belongs to
// chunk -1, not chunk 0. (Division by 16 would round towards zero and put
// blocks -15 to 15 all in chunk 0.)
// Block positions must lie inside the chunk grid (plus or minus 2^35 blocks).
[[nodiscard]] constexpr ChunkPos toChunk(const BlockPos& p) noexcept {
    constexpr i64 kMinBlock = static_cast<i64>(INT32_MIN) * kChunkSize;
    constexpr i64 kMaxBlock = (static_cast<i64>(INT32_MAX) * kChunkSize) + kChunkMask;
    VXE_ASSERT(p.x >= kMinBlock && p.x <= kMaxBlock && p.y >= kMinBlock && p.y <= kMaxBlock &&
                   p.z >= kMinBlock && p.z <= kMaxBlock,
               "BlockPos outside the chunk grid");
    return {static_cast<i32>(p.x >> kChunkShift), static_cast<i32>(p.y >> kChunkShift),
            static_cast<i32>(p.z >> kChunkShift)};
}

// The position of a block inside its chunk. Masking with 15 also gives the
// right answer for negative numbers, because integers are two's complement.
[[nodiscard]] constexpr LocalPos toLocal(const BlockPos& p) noexcept {
    return {static_cast<u8>(p.x & kChunkMask), static_cast<u8>(p.y & kChunkMask),
            static_cast<u8>(p.z & kChunkMask)};
}

// The world position of a block given its chunk and local position. The
// inverse of toChunk plus toLocal.
[[nodiscard]] constexpr BlockPos toBlock(const ChunkPos& chunk, const LocalPos& local) noexcept {
    return {(static_cast<i64>(chunk.x) << kChunkShift) + local.x,
            (static_cast<i64>(chunk.y) << kChunkShift) + local.y,
            (static_cast<i64>(chunk.z) << kChunkShift) + local.z};
}

// The world position of the chunk's minimum corner block.
[[nodiscard]] constexpr BlockPos chunkOrigin(const ChunkPos& chunk) noexcept {
    return toBlock(chunk, LocalPos{});
}

// The block that contains a world position (for example an entity's feet).
// Rounds down on every axis, so -0.5 is in block -1. Positions outside the
// int64 range are clamped, which only happens with corrupt data.
[[nodiscard]] BlockPos blockAt(const DVec3& position) noexcept;

// The largest per axis distance between two chunks, in chunks. Streaming uses
// it because the loaded area around the player is a cube, not a sphere.
[[nodiscard]] constexpr i64 chebyshevDistance(const ChunkPos& a, const ChunkPos& b) noexcept {
    // Computed in int64 so chunks at opposite int32 extremes cannot overflow.
    const auto distance = [](i32 lhs, i32 rhs) {
        const i64 d = static_cast<i64>(lhs) - static_cast<i64>(rhs);
        return d < 0 ? -d : d;
    };
    return std::max({distance(a.x, b.x), distance(a.y, b.y), distance(a.z, b.z)});
}

} // namespace vxe

// Hashing, so ChunkPos can key a HashMap (the chunk map of VoxelWorld).
// The three components go through hashCombine, which ends in a strong mixing
// step, so neighboring chunks spread well across buckets.
template <>
struct std::hash<vxe::ChunkPos> {
    [[nodiscard]] std::size_t operator()(const vxe::ChunkPos& p) const noexcept {
        vxe::u64 h = vxe::hashCombine(0, static_cast<vxe::u32>(p.x));
        h = vxe::hashCombine(h, static_cast<vxe::u32>(p.y));
        return static_cast<std::size_t>(vxe::hashCombine(h, static_cast<vxe::u32>(p.z)));
    }
};

template <>
struct std::hash<vxe::BlockPos> {
    [[nodiscard]] std::size_t operator()(const vxe::BlockPos& p) const noexcept {
        vxe::u64 h = vxe::hashCombine(0, static_cast<vxe::u64>(p.x));
        h = vxe::hashCombine(h, static_cast<vxe::u64>(p.y));
        return static_cast<std::size_t>(vxe::hashCombine(h, static_cast<vxe::u64>(p.z)));
    }
};

// Formatting, so positions can be logged directly: VXE_LOG_INFO(c, "at {}", pos).
template <>
struct std::formatter<vxe::ChunkPos> : std::formatter<std::string_view> {
    auto format(const vxe::ChunkPos& p, std::format_context& context) const {
        return std::format_to(context.out(), "chunk({}, {}, {})", p.x, p.y, p.z);
    }
};

template <>
struct std::formatter<vxe::BlockPos> : std::formatter<std::string_view> {
    auto format(const vxe::BlockPos& p, std::format_context& context) const {
        return std::format_to(context.out(), "block({}, {}, {})", p.x, p.y, p.z);
    }
};

template <>
struct std::formatter<vxe::LocalPos> : std::formatter<std::string_view> {
    auto format(const vxe::LocalPos& p, std::format_context& context) const {
        return std::format_to(context.out(), "local({}, {}, {})", p.x, p.y, p.z);
    }
};
