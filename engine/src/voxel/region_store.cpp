#include "vxe/voxel/region_store.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>
#include <format>
#include <system_error>

namespace vxe::voxel {

RegionStore::RegionStore(const RegionStoreDesc& desc)
    : m_folder(desc.folder)
    , m_stats(desc.stats)
    , m_compressionLevel(desc.settings->registerInt(
          {{.id = "Server.Save.CompressionLevel",
            .displayName = "Save compression",
            .description = "How hard region files are compressed (zstd level). Higher saves disk space and "
                           "costs save time.",
            .visibility = config::Visibility::Advanced},
           io::kDefaultZstdLevel,
           1,
           19,
           1}))
    , m_maxOpenFiles(desc.settings->registerInt(
          {{.id = "Server.Save.MaxOpenRegionFiles",
            .displayName = "Open region files",
            .description = "How many region files stay open between reads and saves.",
            .visibility = config::Visibility::Developer},
           64,
           4,
           1024,
           1})) {
    VXE_ASSERT(desc.settings != nullptr, "RegionStore needs settings");
    if (m_stats == nullptr) {
        return;
    }
    const auto add = [this](const stats::StatDesc& statDesc) {
        auto id = m_stats->registerStat(statDesc);
        return id ? id.value() : stats::StatId{};
    };
    const auto counter = [&](std::string_view name, std::string_view description,
                             stats::StatUnit unit = stats::StatUnit::Count) {
        return add(
            {.name = name, .description = description, .type = stats::StatType::Counter, .unit = unit});
    };
    m_statIds.openFiles = add({.name = "Voxel.Regions.OpenFiles",
                               .description = "Region files open.",
                               .type = stats::StatType::Gauge});
    m_statIds.chunksRead = counter("Voxel.Regions.ChunksRead", "Chunks read from region files.");
    m_statIds.chunksWritten = counter("Voxel.Regions.ChunksWritten", "Chunks written to region files.");
    m_statIds.bytesRead = counter("Voxel.Regions.BytesRead",
                                  "Chunk payload bytes read (before decompression).", stats::StatUnit::Bytes);
    m_statIds.bytesWritten = counter("Voxel.Regions.BytesWritten",
                                     "Chunk payload bytes written (before "
                                     "compression).",
                                     stats::StatUnit::Bytes);
    m_statIds.commits = counter("Voxel.Regions.Commits", "Region file commits.");
    m_statIds.commitTime = add({.name = "Voxel.Regions.CommitTime",
                                .description = "Time spent committing region files, on any thread.",
                                .type = stats::StatType::Timer,
                                .unit = stats::StatUnit::Milliseconds});
}

RegionStore::~RegionStore() = default;

std::filesystem::path RegionStore::fileName(RegionPos region) {
    return std::format("r.{}.{}.{}.vxr", region.x, region.y, region.z);
}

Result<std::shared_ptr<RegionFile>> RegionStore::file(RegionPos region, bool create) {
    const std::scoped_lock lock(m_mutex);
    if (const auto it = m_files.find(region); it != m_files.end()) {
        it->second.lastUse = ++m_useCounter;
        return it->second.file;
    }
    const std::filesystem::path path = m_folder / fileName(region);
    if (!create) {
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            return std::shared_ptr<RegionFile>();
        }
    }
    // Opening reads two sectors and the chunk table; doing it under the lock
    // keeps two threads from opening the same file twice.
    auto opened = RegionFile::open(path, create);
    if (!opened) {
        return std::move(opened).error();
    }
    std::shared_ptr<RegionFile> shared = std::move(opened).value();
    m_files.emplace(region, Open{.file = shared, .lastUse = ++m_useCounter});
    closeUnused(static_cast<usize>(std::max(m_maxOpenFiles.get(), 1)));
    if (m_stats != nullptr && m_statIds.openFiles.isValid()) {
        m_stats->set(m_statIds.openFiles, static_cast<f64>(m_files.size()));
    }
    return shared;
}

// Closes the least recently used files beyond `keep`. Files someone still
// holds stay open (closing happens when their last user lets go). Called
// with m_mutex held.
void RegionStore::closeUnused(usize keep) {
    while (m_files.size() > keep) {
        auto oldest = m_files.end();
        for (auto it = m_files.begin(); it != m_files.end(); ++it) {
            if (it->second.file.use_count() == 1 &&
                (oldest == m_files.end() || it->second.lastUse < oldest->second.lastUse)) {
                oldest = it;
            }
        }
        if (oldest == m_files.end()) {
            return; // Every file is in use.
        }
        m_files.erase(oldest);
    }
}

Result<bool> RegionStore::load(ChunkPos position, std::vector<std::byte>& payload) {
    VXE_PROFILE_FUNCTION();
    payload.clear();
    auto region = file(regionOf(position), false);
    if (!region) {
        return std::move(region).error();
    }
    if (!region.value()) {
        return false;
    }
    Result<bool> found = region.value()->read(regionSlot(position), payload);
    if (found && found.value() && m_stats != nullptr && m_statIds.chunksRead.isValid()) {
        m_stats->add(m_statIds.chunksRead);
        m_stats->add(m_statIds.bytesRead, static_cast<i64>(payload.size()));
    }
    return found;
}

Result<void> RegionStore::save(std::span<const SavedChunk> chunks) {
    VXE_PROFILE_FUNCTION();
    // Group by region: sort a list of indices, then commit each run.
    std::vector<u32> order(chunks.size());
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&chunks](u32 a, u32 b) {
        return regionOf(chunks[a].position) < regionOf(chunks[b].position);
    });
    std::vector<RegionFile::Change> changes;
    for (usize first = 0; first < order.size();) {
        const RegionPos region = regionOf(chunks[order[first]].position);
        usize end = first;
        changes.clear();
        i64 bytes = 0;
        while (end < order.size() && regionOf(chunks[order[end]].position) == region) {
            const SavedChunk& chunk = chunks[order[end]];
            changes.push_back({.slot = regionSlot(chunk.position), .payload = chunk.payload});
            bytes += static_cast<i64>(chunk.payload.size());
            ++end;
        }
        auto target = file(region, true);
        if (!target) {
            return std::move(target).error();
        }
        const Stopwatch stopwatch;
        if (Result<void> committed =
                target.value()->commit(changes, io::Codec::Zstd, m_compressionLevel.get());
            !committed) {
            return committed.error().withContext(
                std::format("Saving region {} {} {}", region.x, region.y, region.z));
        }
        if (m_stats != nullptr && m_statIds.commits.isValid()) {
            m_stats->add(m_statIds.commits);
            m_stats->add(m_statIds.chunksWritten, static_cast<i64>(changes.size()));
            m_stats->add(m_statIds.bytesWritten, bytes);
            m_stats->addTime(m_statIds.commitTime, stopwatch.elapsed());
        }
        first = end;
    }
    return {};
}

usize RegionStore::openFiles() const {
    const std::scoped_lock lock(m_mutex);
    return m_files.size();
}

void RegionStore::closeAll() {
    const std::scoped_lock lock(m_mutex);
    closeUnused(0);
    if (m_stats != nullptr && m_statIds.openFiles.isValid()) {
        m_stats->set(m_statIds.openFiles, static_cast<f64>(m_files.size()));
    }
}

} // namespace vxe::voxel
