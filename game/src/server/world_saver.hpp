#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/chunk_codec.hpp>
#include <vxe/voxel/chunk_source.hpp>
#include <vxe/voxel/region_store.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

namespace vr {

struct WorldSaverDesc {
    std::filesystem::path folder; // saves/<world>/regions.
    vxe::jobs::JobSystem* jobs = nullptr;
    vxe::config::SettingsRegistry* settings = nullptr;
    vxe::stats::StatRegistry* stats = nullptr; // Optional.
    const vxe::voxel::BlockStateNames* names = nullptr;
};

// Keeps a world's edited chunks (Section 21: chunks the generator can make
// again are never saved).
//
// An edited chunk is encoded to memory in the frame of the edit, so it can
// unload at any time without losing anything. save() then commits what is
// waiting on an IO thread. Until a commit has finished its payloads stay in
// memory, and load() prefers them, so a chunk that unloads and comes back
// never reads an older version from disk.
//
// Thread safety: load() may run on any thread, like ChunkSource::produce.
// Everything else belongs to the main thread.
class WorldSaver {
public:
    explicit WorldSaver(const WorldSaverDesc& desc);
    ~WorldSaver();
    WorldSaver(const WorldSaver&) = delete;
    WorldSaver& operator=(const WorldSaver&) = delete;
    WorldSaver(WorldSaver&&) = delete;
    WorldSaver& operator=(WorldSaver&&) = delete;

    void markEdited(vxe::ChunkPos position);
    // Encodes the chunks marked since the last call. Call once per frame,
    // before anything can unload them.
    void encodeEdited(const vxe::voxel::VoxelWorld& world);

    // Fills `out` with the saved chunk at `position`; false if it never was
    // edited, so the generator makes it.
    [[nodiscard]] bool load(vxe::ChunkPos position, vxe::voxel::ChunkProduct& out);

    // Commits what is waiting on an IO thread. Does nothing while one runs.
    void save();
    // Waits until everything is on disk: for leaving the world.
    void flush();
    // Chunks not on disk yet.
    [[nodiscard]] vxe::usize unsavedChunks() const;

private:
    using Payload = std::shared_ptr<const std::vector<std::byte>>;
    struct Saving {
        vxe::ChunkPos position{};
        Payload payload;
    };

    void write();

    vxe::jobs::JobSystem& m_jobs;
    const vxe::voxel::BlockStateNames& m_names;
    vxe::voxel::RegionStore m_store;
    std::vector<vxe::ChunkPos> m_edited;
    vxe::jobs::JobCounter m_writes;
    std::vector<Saving> m_saving; // The running commit's chunks; the job owns it while it runs.
    mutable std::mutex m_mutex;   // Guards m_waiting.
    vxe::HashMap<vxe::ChunkPos, Payload> m_waiting;
};

} // namespace vr
