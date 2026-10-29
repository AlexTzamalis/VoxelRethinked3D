#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <atomic>
#include <filesystem>
#include <optional>

namespace vxe::voxel {

struct LodTileCacheDesc {
    std::filesystem::path folder;         // saves/<world>/lod (Section 21).
    u64 worldHash = 0;                    // lodWorldHash(seed, generator version, preset hash).
    stats::StatRegistry* stats = nullptr; // Optional.
};

// The LOD tile cache of one world on disk (Section 13.4): one LZ4 file per
// tile at l<level>/t.<x>.<z>.vxl (Section 21), made by encodeLodTile.
//
// A cache can always be thrown away, so load() never fails: a missing,
// stale (other world hash or format) or damaged file is a miss, and the
// caller generates the tile and saves it over the old file. Damaged files are
// logged once per cache.
//
// Stats: Voxel.Lod.Cache.Hits, .Misses, .Stale (made for another world,
// generator or preset), .Damaged, .BytesRead and .BytesWritten (file bytes).
//
// Thread safety: load() and save() may be called from any thread at the same
// time (IO threads, Section 6.4), as long as no two threads save the same
// tile at once (the LOD manager owns each tile's work).
class LodTileCache {
public:
    explicit LodTileCache(const LodTileCacheDesc& desc);

    // The cached tile, or nullopt on a miss. Blocks on disk IO.
    [[nodiscard]] std::optional<LodTile> load(const LodTileKey& key);
    // Writes the tile (write to a temporary file, then rename: a crash keeps
    // the old file or the new one). Blocks on disk IO.
    [[nodiscard]] Result<void> save(const LodTile& tile);

    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return m_folder; }
    [[nodiscard]] u64 worldHash() const noexcept { return m_worldHash; }
    // The file of a tile, relative to the cache folder.
    [[nodiscard]] static std::filesystem::path fileName(const LodTileKey& key);

private:
    void count(stats::StatId id, i64 amount = 1);

    std::filesystem::path m_folder;
    u64 m_worldHash;
    stats::StatRegistry* m_stats;
    std::atomic<bool> m_warnedDamaged{false};
    struct StatIds {
        stats::StatId hits;
        stats::StatId misses;
        stats::StatId stale;
        stats::StatId damaged;
        stats::StatId bytesRead;
        stats::StatId bytesWritten;
    } m_statIds;
};

} // namespace vxe::voxel
