#include "vxe/voxel/lod_cache.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/io/file_io.hpp>

#include <format>
#include <vector>

namespace vxe::voxel {

LodTileCache::LodTileCache(const LodTileCacheDesc& desc)
    : m_folder(desc.folder)
    , m_worldHash(desc.worldHash)
    , m_stats(desc.stats) {
    if (m_stats == nullptr) {
        return;
    }
    const auto counter = [this](std::string_view name, std::string_view description,
                                stats::StatUnit unit = stats::StatUnit::Count) {
        auto id = m_stats->registerStat(
            {.name = name, .description = description, .type = stats::StatType::Counter, .unit = unit});
        return id ? id.value() : stats::StatId{};
    };
    m_statIds.hits = counter("Voxel.Lod.Cache.Hits", "LOD tiles loaded from the cache.");
    m_statIds.misses = counter("Voxel.Lod.Cache.Misses", "LOD tiles not in the cache (or not usable).");
    m_statIds.stale =
        counter("Voxel.Lod.Cache.Stale", "Cached LOD tiles made for another generator or preset.");
    m_statIds.damaged = counter("Voxel.Lod.Cache.Damaged", "Cached LOD tiles that could not be read.");
    m_statIds.bytesRead =
        counter("Voxel.Lod.Cache.BytesRead", "LOD cache file bytes read.", stats::StatUnit::Bytes);
    m_statIds.bytesWritten =
        counter("Voxel.Lod.Cache.BytesWritten", "LOD cache file bytes written.", stats::StatUnit::Bytes);
}

std::filesystem::path LodTileCache::fileName(const LodTileKey& key) {
    return std::filesystem::path(std::format("l{}", key.level)) / std::format("t.{}.{}.vxl", key.x, key.z);
}

void LodTileCache::count(stats::StatId id, i64 amount) {
    if (m_stats != nullptr && id.isValid()) {
        m_stats->add(id, amount);
    }
}

std::optional<LodTile> LodTileCache::load(const LodTileKey& key, u64 worldHash) {
    VXE_PROFILE_FUNCTION();
    auto bytes = io::readFile(m_folder / fileName(key));
    if (!bytes) {
        count(m_statIds.misses);
        if (bytes.error().code() != ErrorCode::NotFound) {
            count(m_statIds.damaged);
        }
        return std::nullopt;
    }
    count(m_statIds.bytesRead, static_cast<i64>(bytes.value().size()));
    auto tile = decodeLodTile(bytes.value(), worldHash, key);
    if (tile) {
        count(m_statIds.hits);
        return std::move(tile.value());
    }
    count(m_statIds.misses);
    if (tile.error().code() == ErrorCode::VersionMismatch) {
        count(m_statIds.stale);
        return std::nullopt;
    }
    count(m_statIds.damaged);
    if (!m_warnedDamaged.exchange(true)) {
        VXE_LOG_WARN(log::core, "LOD cache: {} is damaged ({}); it will be generated again",
                     (m_folder / fileName(key)).string(), tile.error().message());
    }
    return std::nullopt;
}

Result<void> LodTileCache::save(const LodTile& tile, u64 worldHash) {
    VXE_PROFILE_FUNCTION();
    std::vector<std::byte> bytes;
    encodeLodTile(tile, worldHash, bytes);
    VXE_TRY(io::writeFile(m_folder / fileName(tile.key()), bytes, io::WriteMode::Atomic));
    count(m_statIds.bytesWritten, static_cast<i64>(bytes.size()));
    return {};
}

} // namespace vxe::voxel
