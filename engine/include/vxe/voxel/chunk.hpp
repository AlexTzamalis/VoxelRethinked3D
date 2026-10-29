#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/chunk_light.hpp>
#include <vxe/voxel/chunk_storage.hpp>

namespace vxe::voxel {

// Why a chunk needs attention. The streaming manager (E9.7) and the saver
// read these and clear them once the work is queued.
inline constexpr u8 kDirtyMesh = 1U << 0; // Its mesh no longer matches its snapshot.
inline constexpr u8 kDirtySave = 1U << 1; // Its own blocks changed since the last save.

// One 16x16x16 chunk of a VoxelWorld (Section 9.5): its blocks, their light
// (Section 14.2), a version counter and dirty flags. The
// pipeline state of Section 11.2 is kept by the StreamingManager, which also
// tracks chunks that are not loaded yet.
//
// The version counts every change to anything a snapshot of this chunk
// reads: its own blocks and light, the border blocks and light of its 26
// neighbors, and neighbors appearing or disappearing. A job remembers the version of the
// snapshot it worked on; if the chunk's version moved on by the time the
// result is back, the result is stale and is thrown away (Section 11.4).
// Versions are stamps from one counter of the whole world, not per chunk
// counts, so a chunk that is unloaded and loaded again never repeats a
// version an old job may still hold.
//
// All changes go through VoxelWorld, which keeps versions, dirty flags,
// neighbors and column summaries consistent; this class only exposes reads
// and the flags. Chunks live in the world's pool, so their addresses stay
// valid until the world removes them.
//
// Thread safety: owned by the thread that owns the world (the server).
class Chunk {
public:
    explicit Chunk(ChunkPos position) noexcept
        : m_position(position) {}

    [[nodiscard]] ChunkPos position() const noexcept { return m_position; }
    [[nodiscard]] const ChunkStorage& blocks() const noexcept { return m_blocks; }
    [[nodiscard]] const ChunkLight& light() const noexcept { return m_light; }
    [[nodiscard]] u64 version() const noexcept { return m_version; }

    [[nodiscard]] u8 dirtyFlags() const noexcept { return m_dirty; }
    [[nodiscard]] bool isDirty(u8 flags) const noexcept { return (m_dirty & flags) != 0; }
    void clearDirty(u8 flags) noexcept { m_dirty = static_cast<u8>(m_dirty & ~flags); }

    // The server tick that last needed this chunk; streaming unloads the
    // longest unused chunks first when over budget.
    [[nodiscard]] u64 lastAccessTick() const noexcept { return m_lastAccessTick; }
    void touch(u64 tick) noexcept { m_lastAccessTick = tick; }

private:
    friend class VoxelWorld;

    // Something a snapshot of this chunk reads changed; `stamp` is the
    // world's next change number.
    void changed(u64 stamp) noexcept {
        m_version = stamp;
        m_dirty = static_cast<u8>(m_dirty | kDirtyMesh);
    }

    ChunkStorage m_blocks;
    ChunkLight m_light;
    ChunkPos m_position;
    u64 m_version = 0;
    u8 m_dirty = 0;
    u64 m_lastAccessTick = 0;
};

} // namespace vxe::voxel
