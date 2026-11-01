#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/chunk_light.hpp>
#include <vxe/voxel/chunk_storage.hpp>

#include <atomic>
#include <optional>

namespace vxe::voxel {

// One chunk the streaming manager asks a ChunkSource for.
class ChunkRequest {
public:
    ChunkRequest(ChunkPos position, const std::atomic<bool>& cancelled) noexcept
        : m_position(position)
        , m_cancelled(&cancelled) {}

    [[nodiscard]] ChunkPos position() const noexcept { return m_position; }
    // True once the chunk left the streaming range. A source may check it
    // between expensive stages and return early; whatever it produced after
    // that is thrown away (Section 11.4).
    [[nodiscard]] bool isCancelled() const noexcept { return m_cancelled->load(std::memory_order_relaxed); }

private:
    ChunkPos m_position;
    const std::atomic<bool>* m_cancelled;
};

// Where a produced chunk came from, for the stage counts and stats.
enum class ChunkOrigin : u8 {
    Generated, // Made by the world generator.
    Loaded,    // Read from a save (region files, E9.8).
};

// What a ChunkSource hands back for one request. The streaming manager
// reuses products between requests, so `blocks` must be overwritten
// completely (assign or fill).
struct ChunkProduct {
    ChunkStorage blocks;
    ChunkOrigin origin = ChunkOrigin::Generated;
    // The generator's estimate of the surface height of the chunk's column,
    // in blocks (Section 9.5, ColumnSummary). Optional.
    std::optional<i32> surfaceEstimate;
    // Light, if the source has it (a save does); otherwise the chunk starts
    // with ChunkLight's default until the light engine lights it.
    std::optional<ChunkLight> light;
};

// Produces the blocks of chunks for the streaming manager (Section 11): the
// world generator, a save file reader, or a reader that falls back to the
// generator. The game supplies it; the engine knows no terrain.
//
// Thread safety: produce() runs on worker threads, many at once for
// different chunks, so it must only read shared state (or synchronize).
// Generators must also be deterministic (Section 12.1), which the engine
// does not check.
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    ChunkSource() = default;
    ChunkSource(const ChunkSource&) = delete;
    ChunkSource& operator=(const ChunkSource&) = delete;
    ChunkSource(ChunkSource&&) = delete;
    ChunkSource& operator=(ChunkSource&&) = delete;

    // Fills `out` with the chunk at request.position(). Uniform chunks (all
    // air above the terrain, all stone deep below) should use
    // out.blocks.fill(), which is nearly free.
    virtual void produce(const ChunkRequest& request, ChunkProduct& out) = 0;

    // About which chunk layer holds the ground of this chunk column, or nothing
    // if the source cannot tell. Streaming loads those layers first: at long
    // distances most wanted chunks are empty sky or solid rock, and the ground
    // at the edge of the view should not wait behind them. A rough answer is
    // fine. Called on the main thread once per column, so it has to be quick.
    [[nodiscard]] virtual std::optional<i32> surfaceLayer(i32 chunkX, i32 chunkZ) const {
        static_cast<void>(chunkX);
        static_cast<void>(chunkZ);
        return std::nullopt;
    }
};

} // namespace vxe::voxel
