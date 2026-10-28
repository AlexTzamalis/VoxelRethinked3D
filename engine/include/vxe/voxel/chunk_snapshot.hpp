#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <array>

namespace vxe::voxel {

// The padded box of a snapshot: the chunk plus a one block border taken from
// its 26 neighbors, 18 blocks on every axis.
inline constexpr i32 kSnapshotSize = kChunkSize + 2;
inline constexpr i32 kSnapshotArea = kSnapshotSize * kSnapshotSize;
inline constexpr i32 kSnapshotVolume = kSnapshotSize * kSnapshotSize * kSnapshotSize;

// The bit of ChunkSnapshot::neighbors for the neighbor at (dx, dy, dz), each
// -1, 0 or 1. The chunk itself is bit 13 (0, 0, 0).
[[nodiscard]] constexpr u32 neighborBit(i32 dx, i32 dy, i32 dz) noexcept {
    return 1U << static_cast<u32>(((dy + 1) * 9) + ((dz + 1) * 3) + (dx + 1));
}

// Bits of all 26 neighbors (every bit of the 3x3x3 cube except the center).
inline constexpr u32 kAllNeighbors = ((1U << 27U) - 1U) & ~neighborBit(0, 0, 0);

// A decompressed copy of one chunk plus a one block border from its 26
// neighbors (Section 9.5): 18 x 18 x 18 states, 11.4 KB. Meshing and light
// jobs read snapshots instead of locking the live world, which is what makes
// them lock free; the world goes on changing while they run, and the version
// tells afterwards whether their result is still current.
//
// Coordinates are chunk local and run from -1 to 16: (0, 0, 0) to (15, 15,
// 15) is the chunk itself, -1 and 16 are the border. The order is Y major
// like chunk storage (y, then z, then x), so a horizontal slice of the box
// is one contiguous run of 324 states.
//
// Border blocks of neighbors that are not loaded hold the fill state the
// snapshot was taken with; `neighbors` says which neighbors were loaded, so
// a mesher can tell real air from missing data.
//
// A plain value type with no heap memory. 11.4 KB is too big for many on
// the stack, so owners keep them in pools or preallocated buffers.
//
// Thread safety: none needed; a job owns its snapshot.
struct ChunkSnapshot {
    ChunkPos position{};
    u64 version = 0;   // The chunk's version when the snapshot was taken.
    u32 neighbors = 0; // neighborBit() of every neighbor that was loaded.
    std::array<BlockStateId, kSnapshotVolume> states{};

    // Index of a chunk local position (-1 to 16 on every axis) in `states`.
    [[nodiscard]] static constexpr usize index(i32 x, i32 y, i32 z) noexcept {
        VXE_ASSERT(x >= -1 && x <= kChunkSize && y >= -1 && y <= kChunkSize && z >= -1 && z <= kChunkSize,
                   "Snapshot position out of range");
        return static_cast<usize>(((y + 1) * kSnapshotArea) + ((z + 1) * kSnapshotSize) + (x + 1));
    }

    [[nodiscard]] BlockStateId at(i32 x, i32 y, i32 z) const noexcept { return states[index(x, y, z)]; }

    [[nodiscard]] bool hasNeighbor(i32 dx, i32 dy, i32 dz) const noexcept {
        return (neighbors & neighborBit(dx, dy, dz)) != 0;
    }
};

} // namespace vxe::voxel
