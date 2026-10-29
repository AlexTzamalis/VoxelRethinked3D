#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/offset_allocator.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace vxe::render {

// The quad groups of one chunk mesh: 3 passes (opaque, cutout, translucent)
// times 6 face directions, in that order. This matches voxel::ChunkMesh;
// render sits below voxel in the module layers (ADR-015), so it takes the
// mesh as plain data instead of including the mesher.
inline constexpr usize kChunkPassCount = 3;
inline constexpr usize kChunkFaceGroups = kChunkPassCount * 6;
// Bytes of one packed quad (voxel::PackedQuad, Section 10.4).
inline constexpr usize kChunkQuadBytes = 8;

enum class ChunkPass : u8 {
    Opaque,
    Cutout,
    Translucent,
};

// One chunk's mesh as the renderer takes it.
struct ChunkGeometry {
    ChunkPos position{};
    std::span<const std::byte> quads{}; // 8 bytes per quad, ordered by group.
    // Group g is quads groupBegin[g] to groupBegin[g + 1] (in quads).
    std::array<u32, kChunkFaceGroups + 1> groupBegin{};
};

struct ChunkRendererDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA16Float;
    rhi::Format depthFormat = rhi::Format::D32Float;
    // The vertex arena all chunk quads live in (Section 8.2). The plan's
    // default is 512 MB; the arena does not grow yet, so a full arena
    // refuses uploads (counted in Render.Chunks.UploadsOutOfMemory).
    usize arenaBytes = usize{256} << 20U;
    u32 maxChunks = 32768;
    // CPU write staging per frame in flight, the hard cap of the upload
    // budget setting.
    usize stagingBytesPerFrame = usize{16} << 20U;
};

// Draws chunk meshes (Section 8.2): every chunk's quads live in one big GPU
// buffer, the vertex arena, sub allocated with an OffsetAllocator; the
// vertex shader builds the corners of each quad from its 8 bytes (vertex
// pulling, render/chunk.vert) with one shared index buffer describing the
// two triangles of a quad. A metadata buffer holds each resident chunk's
// position, which the shader reads through gl_BaseInstance.
//
// Each frame:
//   beginFrame(camera)      new staging memory, finished deferred frees
//   upload(...) / remove()  any number, until the upload byte budget is used
//   recordUploads(list)     in a Transfer pass: staging to arena copies
//   record(list, pass)      in raster passes, with the frame constants bound
//
// Uploads go through a CPU write ring (the staging memory), limited per
// frame by World.Streaming.MaxUploadKBPerFrame so a burst of new chunks
// cannot cause a frame spike; upload() says when the budget is used up and
// the caller retries next frame. Arena ranges and metadata slots of
// replaced or removed chunks are reused only after the GPU finished the
// frames that drew them.
//
// Draws: one indirect multi draw per pass over all non empty face groups,
// rebuilt on the CPU when chunks change. Translucent draws go far to near
// by chunk; the order is rebuilt when the camera moves into another chunk.
// GPU culling (E9.6) replaces the CPU built draw list.
//
// Settings: World.Streaming.MaxUploadKBPerFrame, Graphics.Terrain.AoStrength
// and Debug.Terrain.Wireframe. Stats under Render.Chunks.
//
// Thread safety: main thread only, like the device.
class ChunkRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<ChunkRenderer>> create(const ChunkRendererDesc& desc);
    ~ChunkRenderer();
    ChunkRenderer(const ChunkRenderer&) = delete;
    ChunkRenderer& operator=(const ChunkRenderer&) = delete;
    ChunkRenderer(ChunkRenderer&&) = delete;
    ChunkRenderer& operator=(ChunkRenderer&&) = delete;

    // The block texture array (Section 15.3) the quads' texture layers index,
    // and its sampler. Not owned; must outlive the renderer's draws.
    void setTextures(rhi::TextureHandle textures, rhi::SamplerHandle sampler) noexcept;

    // Starts a frame: new staging memory, releases ranges the GPU is done
    // with, and remembers the camera for the translucent order.
    void beginFrame(const Camera& camera);

    enum class UploadResult : u8 {
        Uploaded,
        OverBudget,  // This frame's upload budget is used up; try again next frame.
        OutOfMemory, // The arena or the chunk slots are full.
    };
    // Makes `geometry` the chunk's mesh, replacing an older one. A mesh
    // without quads removes the chunk. The quads are copied, so the caller
    // may reuse its mesh right away.
    [[nodiscard]] UploadResult upload(const ChunkGeometry& geometry);
    // Stops drawing a chunk. Returns false if it was not resident.
    bool remove(ChunkPos position);
    [[nodiscard]] bool isResident(ChunkPos position) const noexcept;

    // Copies this frame's uploads into the arena. Record in a Transfer pass
    // before the passes that draw chunks.
    void recordUploads(rhi::CommandList& commands);
    // Draws one pass of every resident chunk. Record inside a raster pass
    // with the formats from the desc, after binding the frame constants.
    void record(rhi::CommandList& commands, ChunkPass pass);

    [[nodiscard]] usize residentChunks() const noexcept { return m_chunks.size(); }
    [[nodiscard]] u64 residentQuads() const noexcept { return m_residentQuads; }
    [[nodiscard]] usize arenaUsedBytes() const noexcept;
    [[nodiscard]] usize arenaCapacityBytes() const noexcept { return m_arenaBytes; }
    [[nodiscard]] u32 drawCount(ChunkPass pass) const noexcept;
    [[nodiscard]] usize uploadedBytesThisFrame() const noexcept { return m_stagingUsed; }
    // The buffers behind the draws, for tests and debug views.
    [[nodiscard]] rhi::BufferHandle arenaBuffer() const noexcept { return m_arena; }
    [[nodiscard]] rhi::BufferHandle metadataBuffer() const noexcept { return m_metadata; }
    [[nodiscard]] rhi::BufferHandle drawBuffer() const noexcept { return m_draws; }

private:
    explicit ChunkRenderer(const ChunkRendererDesc& desc);
    struct Resident {
        ChunkPos position{};
        OffsetAllocator::Allocation range{};
        u32 slot = 0;
        std::array<u32, kChunkFaceGroups + 1> groupBegin{};
    };
    struct PendingCopy {
        usize stagingOffset;
        usize arenaOffset;
        usize bytes;
    };
    struct DeferredFree {
        u64 frame;
        OffsetAllocator::Allocation range;
        u32 slot;
    };
    void release(const Resident& chunk);
    void rebuildDraws();
    void publishStats();

    rhi::Device& m_device;
    stats::StatRegistry* m_stats;
    usize m_arenaBytes;
    u32 m_maxChunks;
    rhi::BufferHandle m_arena;
    rhi::BufferHandle m_metadata;
    rhi::BufferHandle m_indices;
    rhi::BufferHandle m_draws;
    rhi::BufferHandle m_params; // Per pass uniforms (AO strength), rewritten when a setting changes.
    Vec4 m_lastParams{-1.0F};
    usize m_drawCapacity = 0; // Indirect commands the draw buffer holds.
    std::array<rhi::PipelineHandle, kChunkPassCount> m_pipelines{};
    std::array<rhi::PipelineHandle, kChunkPassCount> m_wireframePipelines{};
    rhi::TextureHandle m_textures;
    rhi::SamplerHandle m_sampler;
    CpuWriteRing m_staging;
    std::span<std::byte> m_stagingMemory;
    usize m_stagingUsed = 0;
    std::vector<PendingCopy> m_pendingCopies;

    OffsetAllocator m_allocator;
    HashMap<ChunkPos, Resident> m_chunks;
    std::vector<u32> m_freeSlots;
    std::vector<DeferredFree> m_deferred;
    u64 m_frame = 0;
    u64 m_residentQuads = 0;
    bool m_drawsDirty = true;
    ChunkPos m_cameraChunk{};
    // Indirect commands per pass, CPU side copy of the draw buffer.
    std::vector<std::byte> m_drawData;
    std::array<u32, kChunkPassCount> m_drawFirst{};
    std::array<u32, kChunkPassCount> m_drawCounts{};
    std::vector<std::pair<i64, const Resident*>> m_sortScratch;

    config::Setting<i32> m_uploadBudgetKb;
    config::Setting<f32> m_aoStrength;
    config::Setting<bool> m_wireframe;
    struct StatIds {
        stats::StatId chunks;
        stats::StatId quads;
        stats::StatId arenaBytes;
        stats::StatId bytesPerChunk;
        stats::StatId draws;
        stats::StatId uploadBytes;
        stats::StatId overBudget;
        stats::StatId outOfMemory;
    } m_statIds{};
};

// A placeholder block texture array (until texture packs, G2): one 16 x 16
// layer per color with a little per pixel noise so faces read as blocks,
// with mipmaps. Layer i uses colors[i] (RGBA8, red in the low byte).
// Returns the texture and a pixel art sampler (nearest up close, mipmapped
// far away). The caller destroys both.
struct BlockTextures {
    rhi::TextureHandle texture;
    rhi::SamplerHandle sampler;
};
[[nodiscard]] Result<BlockTextures> createPlaceholderBlockTextures(rhi::Device& device,
                                                                   std::span<const u32> colors);

} // namespace vxe::render
