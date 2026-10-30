#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/region_file.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace vxe::voxel {

struct RegionStoreDesc {
    std::filesystem::path folder; // saves/<world>/regions (Section 21).
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
};

// Server.Save.*. The store registers them, and a game registers them at
// startup too, so that its settings menu shows them while no world exists.
struct RegionStoreSettings {
    explicit RegionStoreSettings(config::SettingsRegistry& settings);

    config::Setting<i32> compressionLevel;
    config::Setting<i32> maxOpenFiles;
};

// The region files of one world (Section 21): finds the file of a chunk,
// keeps recently used files open, and turns a batch of saved chunks into one
// commit per region file. Payloads come from encodeChunk; the store does not
// look inside them.
//
// Files are named r.<x>.<y>.<z>.vxr after their region position. Loading a
// chunk whose region file does not exist reports "not saved" without
// creating the file.
//
// Settings: Server.Save.CompressionLevel (zstd level, Section 21 default 3)
// and Server.Save.MaxOpenRegionFiles. Stats: Voxel.Regions.OpenFiles,
// .ChunksRead, .ChunksWritten, .BytesRead, .BytesWritten (payload bytes
// before compression), .Commits and .CommitTime.
//
// Thread safety: load() and save() may be called from any thread, also at
// the same time (load jobs on workers, saving on an IO thread). Saves of the
// same region are serialized by the region file.
class RegionStore {
public:
    explicit RegionStore(const RegionStoreDesc& desc);
    ~RegionStore();
    RegionStore(const RegionStore&) = delete;
    RegionStore& operator=(const RegionStore&) = delete;
    RegionStore(RegionStore&&) = delete;
    RegionStore& operator=(RegionStore&&) = delete;

    // The saved payload of a chunk into `payload`; false if it was never
    // saved (or removed).
    [[nodiscard]] Result<bool> load(ChunkPos position, std::vector<std::byte>& payload);

    // One chunk of a save: its payload, or empty to remove the chunk.
    struct SavedChunk {
        ChunkPos position{};
        std::span<const std::byte> payload{};
    };
    // Writes the chunks, one commit per region file touched: after a crash
    // each region has either all of its chunks of this batch or none. Stops
    // at the first region that fails; earlier regions stay saved.
    [[nodiscard]] Result<void> save(std::span<const SavedChunk> chunks);

    [[nodiscard]] usize openFiles() const;
    // Closes every file not in use right now.
    void closeAll();
    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return m_folder; }
    [[nodiscard]] static std::filesystem::path fileName(RegionPos region);

private:
    // The open file of a region; nullptr (without an error) when `create` is
    // false and the file does not exist.
    [[nodiscard]] Result<std::shared_ptr<RegionFile>> file(RegionPos region, bool create);
    void closeUnused(usize keep);

    std::filesystem::path m_folder;
    stats::StatRegistry* m_stats;
    RegionStoreSettings m_settings;

    struct Open {
        std::shared_ptr<RegionFile> file;
        u64 lastUse = 0;
    };
    mutable std::mutex m_mutex;
    HashMap<RegionPos, Open> m_files;
    u64 m_useCounter = 0;

    struct StatIds {
        stats::StatId openFiles;
        stats::StatId chunksRead;
        stats::StatId chunksWritten;
        stats::StatId bytesRead;
        stats::StatId bytesWritten;
        stats::StatId commits;
        stats::StatId commitTime;
    } m_statIds{};
};

} // namespace vxe::voxel
