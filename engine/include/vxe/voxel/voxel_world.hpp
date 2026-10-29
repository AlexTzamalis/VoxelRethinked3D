#pragma once

#include <vxe/core/containers.hpp>
#include <vxe/core/pool_allocator.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/chunk.hpp>
#include <vxe/voxel/chunk_snapshot.hpp>
#include <vxe/voxel/column_summary.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace vxe::voxel {

struct VoxelWorldConfig {
    // Chunks per page of the chunk pool. A page is allocated when every slot
    // is in use; 1024 chunks is about 64 KB.
    usize chunksPerPage = 1024;
    // Column summaries per page of their pool (about 1 KB each).
    usize columnsPerPage = 256;
};

// The grid of loaded chunks (Section 9.5): chunks from a pool, a hash map
// from ChunkPos to chunk, a summary per chunk column, and snapshots for jobs.
//
// Every change goes through the world, which keeps the derived data right:
// chunk versions (a change on a chunk border also moves the versions of the
// neighbors whose snapshots include that block), dirty flags, the column
// summaries and the storage statistics. Reads of unloaded positions return
// air; writes to them are refused, since only the streaming manager decides
// which chunks exist.
//
// Stats (optional, written by publishStats() and createSnapshot()):
// Voxel.World.Chunks, .UniformChunks, .Palette1Chunks to .Palette8Chunks,
// .RawChunks, .BlockBytes, .FullLightChunks, .LightBytes, .Columns, and per
// frame .Snapshots plus .SnapshotTime.
//
// Thread safety: owned by one thread, the server thread in the game. Jobs
// get snapshots instead of touching the world (Section 6.4).
class VoxelWorld {
public:
    explicit VoxelWorld(const VoxelWorldConfig& config = {}, stats::StatRegistry* stats = nullptr);
    ~VoxelWorld();
    VoxelWorld(const VoxelWorld&) = delete;
    VoxelWorld& operator=(const VoxelWorld&) = delete;
    VoxelWorld(VoxelWorld&&) = delete;
    VoxelWorld& operator=(VoxelWorld&&) = delete;

    // Loads a chunk with the given blocks and light, or replaces those of
    // the chunk already there (for example when generation finishes).
    // Loading is not an edit, so the chunk is not marked for saving.
    Chunk& insertChunk(ChunkPos position, ChunkStorage blocks = {}, ChunkLight light = {});
    // Unloads a chunk. Returns false if it was not loaded.
    bool removeChunk(ChunkPos position);
    void clear();

    [[nodiscard]] Chunk* findChunk(ChunkPos position) noexcept;
    [[nodiscard]] const Chunk* findChunk(ChunkPos position) const noexcept;
    [[nodiscard]] usize chunkCount() const noexcept { return m_chunks.size(); }

    // Calls fn(Chunk&) for every loaded chunk, in no particular order. fn
    // must not insert or remove chunks.
    template <typename Fn>
    void forEachChunk(Fn&& fn) {
        for (auto& entry : m_chunks) {
            fn(*entry.second);
        }
    }

    // The state at a world position, or `unloaded` if its chunk is not loaded.
    [[nodiscard]] BlockStateId getBlock(const BlockPos& position,
                                        BlockStateId unloaded = kAirState) const noexcept;
    // Changes one block, marking its chunk for meshing and saving. Returns
    // false if the chunk is not loaded. Writing the state a block already
    // has changes nothing, not even the version.
    bool setBlock(const BlockPos& position, BlockStateId state);

    // Chunks that were loaded and current (kDirtyMesh clear) and then
    // changed: their own blocks, a border block of a neighbor, or a neighbor
    // loaded or unloaded. Chunks loaded or replaced by insertChunk() are not
    // listed for that load; their loader knows. The streaming manager remeshes
    // the listed chunks and clears the list each frame. Each chunk appears
    // once until its kDirtyMesh flag is cleared again, so the list stays
    // small even if nobody reads it.
    [[nodiscard]] std::span<const ChunkPos> changedChunks() const noexcept { return m_changedChunks; }
    void clearChangedChunks() noexcept { m_changedChunks.clear(); }

    // The packed light (packLight) at a world position, or `unloaded`.
    [[nodiscard]] u8 getLight(const BlockPos& position, u8 unloaded = kOpenSkyLight) const noexcept;
    // Changes the light of one block. Like setBlock it marks the chunk and
    // the neighbors whose snapshot border holds the block, but not for
    // saving: light is derived from blocks. Returns false if the chunk is
    // not loaded.
    bool setLight(const BlockPos& position, u8 packed);
    // Replaces a whole chunk's light, as light jobs deliver it. Only
    // neighbors whose border light actually changed are marked, so relighting
    // a chunk does not remesh 26 others. Returns false if not loaded.
    bool replaceLight(ChunkPos position, ChunkLight light);

    [[nodiscard]] const ColumnSummary* findColumn(ColumnPos position) const noexcept;
    [[nodiscard]] usize columnCount() const noexcept { return m_columns.size(); }
    // Records the generator's surface height for a column. Ignored if no
    // chunk of the column is loaded; columns only exist while they have
    // chunks, so the generator gives the estimate with the first one.
    void setSurfaceEstimate(ColumnPos position, i32 blockY) noexcept;

    // Fills `out` with the chunk and a one block border from its neighbors,
    // blocks and light. Border blocks of neighbors that are not loaded get
    // `unloaded` and `unloadedLight`. Returns false, leaving `out` untouched,
    // if the chunk itself is not loaded.
    // Fast for uniform neighbors (a fill per neighbor), so the common
    // underground and sky cases cost little more than the chunk itself.
    bool createSnapshot(ChunkPos position, ChunkSnapshot& out, BlockStateId unloaded = kAirState,
                        u8 unloadedLight = kOpenSkyLight) const;

    // Chunk counts per storage mode and the bytes of block data, updated
    // on every change.
    struct StorageCounts {
        usize uniform = 0;
        std::array<usize, 4> palette{}; // 1, 2, 4 and 8 bit indices.
        usize raw = 0;
        usize blockBytes = 0; // ChunkStorage::memoryBytes() of every chunk.
        usize fullLight = 0;  // Chunks whose light is not uniform.
        usize lightBytes = 0; // ChunkLight::memoryBytes() of every chunk.
    };
    [[nodiscard]] const StorageCounts& storageCounts() const noexcept { return m_counts; }

    // Writes the gauges. The owner calls it once per tick or frame.
    void publishStats();

private:
    void account(const ChunkStorage& blocks, bool add) noexcept;
    void accountLight(const ChunkLight& light, bool add) noexcept;
    // The block and light parts of createSnapshot; `around` holds the chunk
    // (index 13) and its neighbors in neighborBit order, null if missing.
    static void snapshotBlocks(const std::array<const Chunk*, 27>& around, ChunkSnapshot& out,
                               BlockStateId unloaded) noexcept;
    static void snapshotLight(const std::array<const Chunk*, 27>& around, ChunkSnapshot& out,
                              u8 unloadedLight) noexcept;
    // Marks the neighbors whose snapshot border holds the block at `local`.
    void borderChanged(ChunkPos position, LocalPos local, u64 stamp);
    [[nodiscard]] static std::optional<ChunkPos> neighborPosition(ChunkPos position, i32 dx, i32 dy,
                                                                  i32 dz) noexcept;
    void markChanged(Chunk& chunk, u64 stamp);
    void neighborChanged(ChunkPos position, i32 dx, i32 dy, i32 dz, u64 stamp);
    void neighborsChanged(ChunkPos position, u64 stamp);
    static void raiseHeights(ColumnSummary& column, const Chunk& chunk);
    void addToColumn(const Chunk& chunk);
    void removeFromColumn(ChunkPos position);
    void rescanColumn(ColumnSummary& column, u32 x, u32 z, i32 belowBlockY) const noexcept;
    void destroyChunk(Chunk* chunk) noexcept;
    void registerStats();

    PoolAllocator<Chunk, MemoryTag::Chunks> m_chunkPool;
    PoolAllocator<ColumnSummary, MemoryTag::Chunks> m_columnPool;
    HashMap<ChunkPos, Chunk*> m_chunks;
    HashMap<ColumnPos, ColumnSummary*> m_columns;
    StorageCounts m_counts;
    u64 m_changeCounter = 0; // Source of chunk version stamps.
    std::vector<ChunkPos> m_changedChunks;

    stats::StatRegistry* m_stats;
    struct StatIds {
        stats::StatId chunks;
        stats::StatId uniform;
        std::array<stats::StatId, 4> palette;
        stats::StatId raw;
        stats::StatId blockBytes;
        stats::StatId fullLight;
        stats::StatId lightBytes;
        stats::StatId columns;
        stats::StatId snapshots;
        stats::StatId snapshotTime;
    } m_statIds;
};

} // namespace vxe::voxel
