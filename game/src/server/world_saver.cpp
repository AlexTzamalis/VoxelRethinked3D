#include "world_saver.hpp"

#include <vxe/core/log.hpp>
#include <vxe/io/binary.hpp>

#include <algorithm>
#include <utility>

namespace vr {

WorldSaver::WorldSaver(const WorldSaverDesc& desc)
    : m_jobs(*desc.jobs)
    , m_names(*desc.names)
    , m_store({.folder = desc.folder, .settings = desc.settings, .stats = desc.stats}) {}

WorldSaver::~WorldSaver() {
    m_jobs.wait(m_writes);
}

void WorldSaver::markEdited(vxe::ChunkPos position) {
    m_edited.push_back(position);
}

void WorldSaver::encodeEdited(const vxe::voxel::VoxelWorld& world) {
    if (m_edited.empty()) {
        return;
    }
    std::ranges::sort(m_edited);
    const auto repeats = std::ranges::unique(m_edited);
    m_edited.erase(repeats.begin(), repeats.end());
    for (const vxe::ChunkPos position : m_edited) {
        const vxe::voxel::Chunk* chunk = world.findChunk(position);
        if (chunk == nullptr) {
            continue;
        }
        vxe::io::BinaryWriter writer;
        vxe::voxel::encodeChunk(chunk->blocks(), m_names, writer);
        auto payload = std::make_shared<const std::vector<std::byte>>(writer.takeBytes());
        const std::scoped_lock lock(m_mutex);
        m_waiting.insert_or_assign(position, std::move(payload));
    }
    m_edited.clear();
}

bool WorldSaver::load(vxe::ChunkPos position, vxe::voxel::ChunkProduct& out) {
    Payload waiting;
    {
        const std::scoped_lock lock(m_mutex);
        if (const auto found = m_waiting.find(position); found != m_waiting.end()) {
            waiting = found->second;
        }
    }
    // Per worker, so loading a chunk allocates only when a payload is bigger
    // than every one before it on this thread.
    thread_local std::vector<std::byte> payload;
    std::span<const std::byte> bytes;
    if (waiting != nullptr) {
        bytes = *waiting;
    } else {
        const auto saved = m_store.load(position, payload);
        if (!saved) {
            VXE_LOG_WARN(vxe::log::core, "Chunk {} {} {} could not be read: {}", position.x, position.y,
                         position.z, saved.error().describe());
            return false;
        }
        if (!saved.value()) {
            return false;
        }
        bytes = payload;
    }
    auto decoded = vxe::voxel::decodeChunk(bytes, m_names);
    if (!decoded) {
        VXE_LOG_WARN(vxe::log::core, "Chunk {} {} {} is damaged: {}", position.x, position.y, position.z,
                     decoded.error().describe());
        return false;
    }
    out.blocks = std::move(decoded.value().blocks);
    out.origin = vxe::voxel::ChunkOrigin::Loaded;
    return true;
}

void WorldSaver::save() {
    m_jobs.wait(m_writes); // Only waits if a commit is nearly done or the disk is slow.
    m_saving.clear();
    {
        const std::scoped_lock lock(m_mutex);
        m_saving.reserve(m_waiting.size());
        for (const auto& [position, payload] : m_waiting) {
            m_saving.push_back({.position = position, .payload = payload});
        }
    }
    if (m_saving.empty()) {
        return;
    }
    m_jobs.schedule(m_writes, [this] { write(); }, {vxe::jobs::Priority::Normal, {}, vxe::jobs::Pool::Io});
}

void WorldSaver::write() {
    std::vector<vxe::voxel::RegionStore::SavedChunk> chunks;
    chunks.reserve(m_saving.size());
    for (const Saving& saving : m_saving) {
        chunks.push_back({.position = saving.position, .payload = *saving.payload});
    }
    if (const auto saved = m_store.save(chunks); !saved) {
        // The payloads stay waiting, so the next save tries again.
        VXE_LOG_WARN(vxe::log::core, "Saving chunks failed: {}", saved.error().describe());
        return;
    }
    // A chunk edited again during the commit has a newer payload: it stays.
    const std::scoped_lock lock(m_mutex);
    for (const Saving& saving : m_saving) {
        if (const auto found = m_waiting.find(saving.position);
            found != m_waiting.end() && found->second == saving.payload) {
            m_waiting.erase(found);
        }
    }
}

void WorldSaver::flush() {
    save();
    m_jobs.wait(m_writes);
    if (unsavedChunks() > 0) {
        // Edited during the running commit, or that commit failed: one more try.
        save();
        m_jobs.wait(m_writes);
    }
}

vxe::usize WorldSaver::unsavedChunks() const {
    const std::scoped_lock lock(m_mutex);
    return m_waiting.size();
}

} // namespace vr
