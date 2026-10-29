#include "vxe/voxel/region_file.hpp"

#include <vxe/core/hash.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/io/binary.hpp>

#include <algorithm>
#include <format>

namespace vxe::voxel {
namespace {

constexpr u32 kMagic = 0x47525856; // "VXRG" in file order (little endian).
constexpr u32 kSuperblockSectors = 2;
constexpr usize kSuperblockBytes = 64; // Used part of a superblock sector; the checksum is the last 8.
constexpr usize kEntryBytes = 24;      // One chunk table entry.

struct Superblock {
    u64 sequence = 0;
    u32 tableSector = 0;
    u32 tableBytes = 0;
    u64 tableChecksum = 0;
    u32 chunks = 0;
};

std::array<std::byte, RegionFile::kSectorBytes> encodeSuperblock(const Superblock& block) {
    io::BinaryWriter writer(kSuperblockBytes);
    writer.writeU32(kMagic);
    writer.writeU16(RegionFile::kFormatVersion);
    writer.writeU16(static_cast<u16>(std::countr_zero(RegionFile::kSectorBytes)));
    writer.writeU64(block.sequence);
    writer.writeU32(block.tableSector);
    writer.writeU32(block.tableBytes);
    writer.writeU64(block.tableChecksum);
    writer.writeU32(block.chunks);
    while (writer.size() < kSuperblockBytes - sizeof(u64)) {
        writer.writeU8(0);
    }
    writer.writeU64(hashBytes(writer.bytes()));
    std::array<std::byte, RegionFile::kSectorBytes> sector{};
    std::ranges::copy(writer.bytes(), sector.begin());
    return sector;
}

enum class SlotState : u8 {
    Valid,
    NeverWritten, // Zeros, or past the end of the file.
    Invalid,      // Written but not valid: a torn write, or damage.
};

SlotState decodeSuperblock(std::span<const std::byte> sector, Superblock& out) {
    if (std::ranges::all_of(sector, [](std::byte value) { return value == std::byte{0}; })) {
        return SlotState::NeverWritten;
    }
    const std::span<const std::byte> used = sector.first(kSuperblockBytes);
    io::BinaryReader reader(used);
    const u32 magic = reader.readU32();
    const u16 version = reader.readU16();
    const u16 sectorShift = reader.readU16();
    out.sequence = reader.readU64();
    out.tableSector = reader.readU32();
    out.tableBytes = reader.readU32();
    out.tableChecksum = reader.readU64();
    out.chunks = reader.readU32();
    io::BinaryReader tail(used.subspan(kSuperblockBytes - sizeof(u64)));
    const u64 checksum = tail.readU64();
    if (!reader.ok() || magic != kMagic ||
        checksum != hashBytes(used.first(kSuperblockBytes - sizeof(u64))) ||
        version != RegionFile::kFormatVersion ||
        sectorShift != static_cast<u16>(std::countr_zero(RegionFile::kSectorBytes))) {
        return SlotState::Invalid;
    }
    return SlotState::Valid;
}

constexpr u32 sectorsFor(u64 bytes) noexcept {
    return static_cast<u32>((bytes + RegionFile::kSectorBytes - 1) / RegionFile::kSectorBytes);
}

constexpr u64 offsetOf(u32 sector) noexcept {
    return u64{sector} * RegionFile::kSectorBytes;
}

// First fit search for `count` free sectors; past the end of the file when
// nothing fits. Marks them used.
u32 allocate(std::vector<bool>& used, u32 count) {
    u32 run = 0;
    for (u32 sector = kSuperblockSectors; sector < used.size(); ++sector) {
        run = used[sector] ? 0 : run + 1;
        if (run == count) {
            const u32 first = sector + 1 - count;
            std::fill_n(used.begin() + first, count, true);
            return first;
        }
    }
    // Extend the trailing free run (if any) past the end.
    const auto first = static_cast<u32>(used.size()) - run;
    used.resize(usize{first} + count, true);
    std::fill_n(used.begin() + first, count, true);
    return first;
}

} // namespace

Result<std::unique_ptr<RegionFile>> RegionFile::open(const std::filesystem::path& path, bool create) {
    auto file =
        io::RandomAccessFile::open(path, create ? io::OpenMode::ReadWriteOrCreate : io::OpenMode::ReadWrite);
    if (!file) {
        return std::move(file).error();
    }
    std::unique_ptr<RegionFile> region(new RegionFile(std::move(file).value()));
    VXE_TRY(region->load());
    return region;
}

void RegionFile::markUsed(State& state) {
    state.used.assign(kSuperblockSectors, true);
    const auto mark = [&state](u32 first, u32 bytes) {
        const u32 count = sectorsFor(bytes);
        if (state.used.size() < usize{first} + count) {
            state.used.resize(usize{first} + count, false);
        }
        std::fill_n(state.used.begin() + first, count, true);
    };
    for (const Entry& entry : state.entries) {
        if (entry.present) {
            mark(entry.firstSector, entry.storedBytes);
        }
    }
    if (state.tableBytes > 0) {
        mark(state.tableSector, state.tableBytes);
    }
}

// Reads the newest valid commit. Called when opening, and after a failed
// commit so memory matches whatever the disk now holds.
Result<void> RegionFile::load() {
    VXE_PROFILE_FUNCTION();
    auto fileSize = m_file->size();
    if (!fileSize) {
        return std::move(fileSize).error();
    }
    const u64 size = fileSize.value();
    std::array<Superblock, kSuperblockSectors> blocks{};
    std::array<SlotState, kSuperblockSectors> states{};
    for (u32 slot = 0; slot < kSuperblockSectors; ++slot) {
        std::array<std::byte, kSectorBytes> sector{};
        const u64 available = size > offsetOf(slot) ? std::min<u64>(size - offsetOf(slot), kSectorBytes) : 0;
        if (available > 0) {
            VXE_TRY(m_file->read(offsetOf(slot), std::span(sector).first(static_cast<usize>(available))));
        }
        states.at(slot) = decodeSuperblock(sector, blocks.at(slot));
    }
    // Newest first; a slot whose chunk table does not check out is skipped
    // like a torn superblock.
    std::array<u32, kSuperblockSectors> order = {0, 1};
    if (states[1] == SlotState::Valid &&
        (states[0] != SlotState::Valid || blocks[1].sequence > blocks[0].sequence)) {
        order = {1, 0};
    }
    std::string problems;
    for (const u32 slot : order) {
        if (states.at(slot) != SlotState::Valid) {
            continue;
        }
        const Superblock& block = blocks.at(slot);
        auto state = std::make_unique<State>();
        state->sequence = block.sequence;
        state->tableSector = block.tableSector;
        state->tableBytes = block.tableBytes;
        if (block.tableSector < kSuperblockSectors || offsetOf(block.tableSector) + block.tableBytes > size) {
            problems += std::format(" slot {}: chunk table outside the file;", slot);
            continue;
        }
        std::vector<std::byte> table(block.tableBytes);
        VXE_TRY(m_file->read(offsetOf(block.tableSector), table));
        if (hashBytes(table) != block.tableChecksum) {
            problems += std::format(" slot {}: chunk table checksum mismatch;", slot);
            continue;
        }
        io::BinaryReader reader(table);
        const u32 count = reader.readU32();
        bool good = reader.ok() && count == block.chunks && count <= kRegionChunks;
        for (u32 i = 0; good && i < count; ++i) {
            const u16 chunkSlot = reader.readU16();
            const auto codec = static_cast<io::Codec>(reader.readU8());
            static_cast<void>(reader.readU8());
            const Entry entry{.firstSector = reader.readU32(),
                              .storedBytes = reader.readU32(),
                              .rawBytes = reader.readU32(),
                              .checksum = reader.readU64(),
                              .codec = codec,
                              .present = true};
            good = reader.ok() && chunkSlot < kRegionChunks && !state->entries.at(chunkSlot).present &&
                   codec <= io::Codec::Lz4 && entry.firstSector >= kSuperblockSectors &&
                   offsetOf(entry.firstSector) + entry.storedBytes <= size;
            if (good) {
                state->entries.at(chunkSlot) = entry;
            }
        }
        if (!good || !reader.atEnd()) {
            problems += std::format(" slot {}: malformed chunk table;", slot);
            continue;
        }
        state->chunks = count;
        markUsed(*state);
        const std::unique_lock lock(m_stateMutex);
        m_state = std::move(state);
        return {};
    }
    if (states[0] == SlotState::Valid || states[1] == SlotState::Valid ||
        (states[0] == SlotState::Invalid && states[1] == SlotState::Invalid)) {
        return Error(ErrorCode::ParseError,
                     std::format("Region file '{}' has no readable commit:{}", m_file->path().string(),
                                 problems.empty() ? " both superblocks are damaged" : problems));
    }
    // Nothing was ever committed completely: an empty region.
    auto empty = std::make_unique<State>();
    markUsed(*empty);
    const std::unique_lock lock(m_stateMutex);
    m_state = std::move(empty);
    return {};
}

Result<void> RegionFile::write(u64 offset, std::span<const std::byte> data) {
    if (m_bytesWritten + data.size() > m_failAt) {
        // Simulated crash: part of the data reaches the file, then nothing.
        const u64 allowed = m_failAt > m_bytesWritten ? m_failAt - m_bytesWritten : 0;
        if (allowed > 0) {
            VXE_TRY(m_file->write(offset, data.first(static_cast<usize>(allowed))));
            m_bytesWritten += allowed;
        }
        return Error(ErrorCode::IoError, "Simulated crash while writing (failWritesAfter)");
    }
    VXE_TRY(m_file->write(offset, data));
    m_bytesWritten += data.size();
    return {};
}

Result<bool> RegionFile::read(u16 slot, std::vector<std::byte>& out) const {
    VXE_PROFILE_FUNCTION();
    out.clear();
    VXE_ASSERT(slot < kRegionChunks, "Region slot out of range");
    thread_local std::vector<std::byte> stored;
    Entry entry;
    {
        const std::shared_lock lock(m_stateMutex);
        entry = m_state->entries.at(slot);
        if (!entry.present) {
            return false;
        }
        stored.resize(entry.storedBytes);
        VXE_TRY(m_file->read(offsetOf(entry.firstSector), stored));
    }
    if (hashBytes(stored) != entry.checksum) {
        return Error(ErrorCode::ParseError,
                     std::format("Chunk {} of region file '{}' is damaged (checksum mismatch)", slot,
                                 m_file->path().string()));
    }
    if (entry.codec == io::Codec::None) {
        out = stored;
        return true;
    }
    out.resize(entry.rawBytes);
    if (Result<void> unpacked = io::decompressInto(stored, out, entry.codec); !unpacked) {
        out.clear();
        return unpacked.error().withContext(
            std::format("Chunk {} of region file '{}'", slot, m_file->path().string()));
    }
    return true;
}

Result<void> RegionFile::commit(std::span<const Change> changes, io::Codec codec, int level) {
    VXE_PROFILE_FUNCTION();
    const std::scoped_lock commitLock(m_commitMutex);
    // Compress first: the slow part needs no lock at all.
    struct Prepared {
        u16 slot;
        std::vector<std::byte> stored;
        u32 rawBytes;
    };
    std::vector<Prepared> prepared;
    prepared.reserve(changes.size());
    for (const Change& change : changes) {
        VXE_ASSERT(change.slot < kRegionChunks, "Region slot out of range");
        Prepared item{.slot = change.slot, .stored = {}, .rawBytes = static_cast<u32>(change.payload.size())};
        if (!change.payload.empty()) {
            if (codec == io::Codec::None) {
                item.stored.assign(change.payload.begin(), change.payload.end());
            } else {
                auto packed = io::compress(change.payload, codec, level);
                if (!packed) {
                    return std::move(packed).error();
                }
                item.stored = std::move(packed).value();
            }
        }
        prepared.push_back(std::move(item));
    }

    // The next state starts as a copy of the last commit; new data only goes
    // where the last commit has nothing (its `used` sectors stay untouched).
    auto next = std::make_unique<State>(*m_state);
    std::vector<bool> taken = m_state->used;
    const auto fail = [this](Error error) -> Result<void> {
        // Whatever reached the disk, memory must match what a reopen sees.
        static_cast<void>(load());
        return error;
    };
    for (const Prepared& item : prepared) {
        Entry& entry = next->entries.at(item.slot);
        if (item.stored.empty()) {
            entry = {};
            continue;
        }
        const u32 first = allocate(taken, sectorsFor(item.stored.size()));
        if (Result<void> written = write(offsetOf(first), item.stored); !written) {
            return fail(written.error());
        }
        entry = {.firstSector = first,
                 .storedBytes = static_cast<u32>(item.stored.size()),
                 .rawBytes = item.rawBytes,
                 .checksum = hashBytes(item.stored),
                 .codec = codec,
                 .present = true};
    }

    io::BinaryWriter table(sizeof(u32) + (kRegionChunks * kEntryBytes));
    u32 chunks = 0;
    for (const Entry& entry : next->entries) {
        chunks += entry.present ? 1U : 0U;
    }
    table.writeU32(chunks);
    for (u32 slot = 0; slot < kRegionChunks; ++slot) {
        const Entry& entry = next->entries.at(slot);
        if (!entry.present) {
            continue;
        }
        table.writeU16(static_cast<u16>(slot));
        table.writeU8(static_cast<u8>(entry.codec));
        table.writeU8(0);
        table.writeU32(entry.firstSector);
        table.writeU32(entry.storedBytes);
        table.writeU32(entry.rawBytes);
        table.writeU64(entry.checksum);
    }
    next->tableSector = allocate(taken, sectorsFor(table.size()));
    next->tableBytes = static_cast<u32>(table.size());
    next->chunks = chunks;
    next->sequence = m_state->sequence + 1;
    if (Result<void> written = write(offsetOf(next->tableSector), table.bytes()); !written) {
        return fail(written.error());
    }
    if (Result<void> synced = m_file->sync(); !synced) {
        return fail(synced.error());
    }
    const auto sector = encodeSuperblock({.sequence = next->sequence,
                                          .tableSector = next->tableSector,
                                          .tableBytes = next->tableBytes,
                                          .tableChecksum = hashBytes(table.bytes()),
                                          .chunks = chunks});
    if (Result<void> written = write(offsetOf(static_cast<u32>(next->sequence % kSuperblockSectors)), sector);
        !written) {
        return fail(written.error());
    }
    if (Result<void> synced = m_file->sync(); !synced) {
        return fail(synced.error());
    }
    markUsed(*next);
    const std::unique_lock lock(m_stateMutex);
    m_state = std::move(next);
    return {};
}

bool RegionFile::contains(u16 slot) const {
    const std::shared_lock lock(m_stateMutex);
    return m_state->entries.at(slot).present;
}

usize RegionFile::chunkCount() const {
    const std::shared_lock lock(m_stateMutex);
    return m_state->chunks;
}

u64 RegionFile::sequence() const {
    const std::shared_lock lock(m_stateMutex);
    return m_state->sequence;
}

usize RegionFile::usedSectors() const {
    const std::shared_lock lock(m_stateMutex);
    return static_cast<usize>(std::ranges::count(m_state->used, true));
}

} // namespace vxe::voxel
