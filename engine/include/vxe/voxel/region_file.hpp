#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/io/compression.hpp>
#include <vxe/io/random_access_file.hpp>
#include <vxe/math/coords.hpp>

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

namespace vxe::voxel {

// Regions group 16 x 16 x 16 chunks into one file (Section 21).
inline constexpr i32 kRegionShift = 4;
inline constexpr u32 kRegionChunks = 16 * 16 * 16;

struct RegionPos {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    friend constexpr auto operator<=>(const RegionPos&, const RegionPos&) = default;
};

// The region holding a chunk. The arithmetic shift rounds towards negative
// infinity, so chunk -1 is in region -1, like blocks in chunks.
[[nodiscard]] constexpr RegionPos regionOf(ChunkPos chunk) noexcept {
    return {.x = chunk.x >> kRegionShift, .y = chunk.y >> kRegionShift, .z = chunk.z >> kRegionShift};
}

// The chunk's slot in its region file, y major like blocks in chunks.
[[nodiscard]] constexpr u16 regionSlot(ChunkPos chunk) noexcept {
    constexpr i32 kMask = (1 << kRegionShift) - 1;
    return static_cast<u16>(((chunk.y & kMask) << 8) | ((chunk.z & kMask) << 4) | (chunk.x & kMask));
}

// One region file: up to 4096 chunk payloads in 512 byte sectors (Section 21).
//
// Layout:
//   sector 0, sector 1   two superblocks: magic, format version, a sequence
//                        number, where the chunk table is and its checksum,
//                        and a checksum of the superblock itself
//   any other sectors    chunk payloads (compressed) and the chunk table: per
//                        stored chunk its slot, first sector, stored and raw
//                        size, codec and the checksum of the stored bytes
//
// Crash safety. A commit never overwrites anything the last commit uses: new
// payloads and the new chunk table go into free sectors (or the end of the
// file), the file is synced, then the new superblock goes into the slot the
// last commit did not use, with the next sequence number, and the file is
// synced again. Opening takes the valid superblock with the highest
// sequence. A crash at any point leaves the last complete commit readable:
// before the superblock is complete the new one fails its checksum and the
// old one still points at untouched sectors. A slot of zeros was never
// written; a file with no valid superblock and no written slot is an empty
// region (a crash during its first commit). Payloads carry checksums too, so
// damage from outside (a bad disk) shows as an error instead of wrong blocks.
//
// Sectors freed by a commit are reused by the next ones; the file does not
// shrink (compaction can come later if saves grow).
//
// Thread safety: read() from any number of threads, also while a commit()
// runs on another; commits are serialized. A reader holds a shared lock while
// it reads sectors, and a commit only frees sectors under the exclusive lock,
// so a reader never sees its sectors reused.
class RegionFile {
public:
    static constexpr u32 kSectorBytes = 512;
    static constexpr u16 kFormatVersion = 1;

    // Opens a region file, creating an empty one with `create`. Fails with
    // NotFound if it does not exist and `create` is false, and with
    // ParseError if no valid superblock can be found in a written file.
    [[nodiscard]] static Result<std::unique_ptr<RegionFile>> open(const std::filesystem::path& path,
                                                                  bool create);

    // One change of a commit: a new payload for a slot, or (empty) its removal.
    struct Change {
        u16 slot = 0;
        std::span<const std::byte> payload{};
    };

    // Reads and decompresses the payload of a slot into `out`. Returns false
    // (and leaves `out` empty) if the slot holds nothing.
    [[nodiscard]] Result<bool> read(u16 slot, std::vector<std::byte>& out) const;
    // Writes all changes as one commit: after a crash either all of them or
    // none are in the file. Payloads are compressed with `codec` first.
    [[nodiscard]] Result<void> commit(std::span<const Change> changes, io::Codec codec = io::Codec::Zstd,
                                      int level = io::kDefaultZstdLevel);

    [[nodiscard]] bool contains(u16 slot) const;
    [[nodiscard]] usize chunkCount() const;
    // The sequence number of the last commit (0 for a new file).
    [[nodiscard]] u64 sequence() const;
    // Sectors the last commit uses, including both superblocks.
    [[nodiscard]] usize usedSectors() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_file->path(); }

    // Bytes written since opening, and a test hook: once that many more
    // bytes were written, every write fails after writing only part of its
    // data, which is what a crash in the middle of a commit leaves behind.
    [[nodiscard]] u64 bytesWritten() const noexcept { return m_bytesWritten; }
    void failWritesAfter(u64 bytes) noexcept { m_failAt = m_bytesWritten + bytes; }

private:
    struct Entry {
        u32 firstSector = 0;
        u32 storedBytes = 0;
        u32 rawBytes = 0;
        u64 checksum = 0;
        io::Codec codec = io::Codec::None;
        bool present = false;
    };
    struct State {
        std::array<Entry, kRegionChunks> entries{};
        u64 sequence = 0;
        u32 tableSector = 0;
        u32 tableBytes = 0;
        usize chunks = 0;
        std::vector<bool> used; // Per sector, used by this state (both superblocks always).
    };

    explicit RegionFile(std::unique_ptr<io::RandomAccessFile> file) noexcept
        : m_file(std::move(file)) {}
    [[nodiscard]] Result<void> load();
    [[nodiscard]] Result<void> write(u64 offset, std::span<const std::byte> data);
    static void markUsed(State& state);

    std::unique_ptr<io::RandomAccessFile> m_file;
    std::unique_ptr<State> m_state; // The last commit. Large, so on the heap.
    mutable std::shared_mutex m_stateMutex;
    std::mutex m_commitMutex;
    u64 m_bytesWritten = 0;
    u64 m_failAt = std::numeric_limits<u64>::max();
};

} // namespace vxe::voxel

// Hashing so regions can key hash maps (the region store's open files).
template <>
struct std::hash<vxe::voxel::RegionPos> {
    [[nodiscard]] std::size_t operator()(const vxe::voxel::RegionPos& position) const noexcept {
        return std::hash<vxe::ChunkPos>{}(vxe::ChunkPos{.x = position.x, .y = position.y, .z = position.z});
    }
};
