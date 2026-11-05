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
#include <vxe/render/chunk_culling.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/render/hi_z.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vxe::render {

// The quad groups of one chunk mesh: 4 passes (opaque, cutout, plants,
// translucent) times 6 face directions, in that order. This matches voxel::ChunkMesh;
// render sits below voxel in the module layers (ADR-015), so it takes the
// mesh as plain data instead of including the mesher.
inline constexpr usize kChunkPassCount = 4;
inline constexpr usize kChunkFaceGroups = kChunkPassCount * 6;
// Bytes of one packed quad (voxel::PackedQuad, Section 10.4).
inline constexpr usize kChunkQuadBytes = 12;

enum class ChunkPass : u8 {
    Opaque,
    Cutout,
    Plants, // Alpha tested like cutout, seen from both sides.
    Translucent,
};

// One chunk's mesh as the renderer takes it.
struct ChunkGeometry {
    ChunkPos position{};
    std::span<const std::byte> quads{}; // 8 bytes per quad, ordered by group.
    // Group g is quads groupBegin[g] to groupBegin[g + 1] (in quads).
    std::array<u32, kChunkFaceGroups + 1> groupBegin{};
    // Which pairs of the chunk's faces are joined by non opaque blocks
    // (voxel::ChunkMesh::connectivity), for cave culling.
    u16 connectivity = kChunkAllConnected;
    // The tight box around the quads (voxel::ChunkMesh::boundsMin and
    // boundsMax), for the frustum, direction and occlusion tests.
    ChunkBounds bounds{};
};

struct ChunkRendererDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA16Float;
    rhi::Format depthFormat = rhi::Format::D32Float;
    // The vertex arena all chunk quads live in (Section 8.2). It does not grow,
    // so a full arena refuses uploads (counted in Render.Chunks.UploadsOutOfMemory)
    // and streaming pulls real chunks in until they fit. Sized for render
    // distance 32 over leafy terrain, where a chunk can hold 13 KB of quads.
    usize arenaBytes = usize{1024} << 20U;
    u32 maxChunks = 131072;
    // CPU write staging per frame in flight, the hard cap of the upload
    // budget setting.
    usize stagingBytesPerFrame = usize{16} << 20U;
};

// What the latest culling did with the face groups of all resident chunks.
// GPU results arrive a few frames late (read without stalling).
struct ChunkCullingCounts {
    u32 candidates = 0; // Non empty face groups of resident chunks.
    u32 visible = 0;
    u32 frustum = 0;   // Outside the view.
    u32 direction = 0; // Every face pointing away from the camera.
    u32 cave = 0;      // Not reached by the cave search.
    u32 occlusion = 0; // Hidden behind last frame's depth.
    bool onGpu = false;
};

// Draws chunk meshes (Section 8.2): every chunk's quads live in one big GPU
// buffer, the vertex arena, sub allocated with an OffsetAllocator; the
// vertex shader builds the corners of each quad from its 8 bytes (vertex
// pulling, render/chunk.vert) with one shared index buffer describing the
// two triangles of a quad. A metadata buffer holds each resident chunk's
// position and bounds, which the shaders read through gl_BaseInstance.
//
// Each frame:
//   beginFrame(camera)      new staging memory, finished deferred frees
//   upload(...) / remove()  any number, until the upload byte budget is used
//   recordUploads(list)     in a Transfer pass: staging to arena copies
//   recordCulling(list)     in a Compute pass: decides which groups to draw
//   record(list, pass)      in raster passes, with the frame constants bound
//   recordOcclusionDepth()  in a Compute pass after the opaque depth exists
//
// Uploads go through a CPU write ring (the staging memory), limited per
// frame by World.Streaming.MaxUploadKBPerFrame so a burst of new chunks
// cannot cause a frame spike; upload() says when the budget is used up and
// the caller retries next frame. Arena ranges and metadata slots of
// replaced or removed chunks are reused only after the GPU finished the
// frames that drew them.
//
// Culling (Section 8.3). Every non empty face group of every resident chunk
// is a candidate draw; a chunk change only adds or removes that chunk's
// candidates in the list (one region per pass), and only the changed entries
// are copied to the GPU. Each frame the candidates go through, in order:
//   frustum     Graphics.Culling.Frustum, with Graphics.Culling.FrustumPadding
//   direction   Graphics.Terrain.FaceDirectionCulling (not for translucent)
//   cave        Graphics.Culling.CaveCulling: CaveCuller over the chunks'
//               connectivity, searched again (on a job, setJobSystem) when
//               the camera changes chunk or chunks change
//   occlusion   Graphics.Culling.Occlusion: the chunk's box against the
//               depth pyramid (HiZPyramid) built from last frame's depth
// On the GPU (Graphics.Culling.Gpu, when the driver has indirect count
// draws) a compute shader writes the surviving draws and their count, so the
// CPU never waits for the result. Otherwise the CPU runs the same tests
// except occlusion and writes the draws itself. If recordCulling was not
// called in a frame, every candidate is drawn.
//
// Debug.FreezeCulling keeps the camera the culling uses (and the depth
// pyramid), so one can fly around and look at what was culled.
//
// Translucent draws go far to near by chunk; the order is sorted again when
// the camera moves into another chunk or a chunk with translucent quads
// changes.
//
// Light (Section 14): each quad carries the sky and block light of the cell
// in front of it; the shader shows the brighter of sky light times the
// daylight (setDaylight) and warm block light, never darker than
// Graphics.Lighting.Brightness.
//
// Settings: World.Streaming.MaxUploadKBPerFrame, Graphics.Terrain.AoStrength,
// Graphics.Lighting.Brightness,
// Graphics.Terrain.FaceDirectionCulling, Graphics.Culling.*,
// Debug.FreezeCulling and Debug.Terrain.Wireframe. Stats under
// Render.Chunks and Render.Culling.
//
// Thread safety: main thread only, like the device.
// Tint kinds of block textures, kind 0 being no tint (voxel::kTintCount).
inline constexpr usize kBlockTintKinds = 16;

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
    // Runs the cave search on `jobs` (CaveCuller::setJobSystem), or on the
    // main thread with nullptr. Set nullptr before the job system goes away.
    void setJobSystem(jobs::JobSystem* jobs) { m_caves.setJobSystem(jobs); }
    // How strong sky light is right now, 0 (no sun) to 1 (full day). The
    // renderer sets it from the time of day every frame; quads keep their
    // sky light level, and this scales its brightness.
    void setDaylight(f32 daylight) noexcept;
    // The color of sky light on terrain (SkyParams::skyLight).
    void setSkyLight(const Vec3& color) noexcept { m_skyLight = color; }
    // How wet open ground looks, 0 dry to 1 soaked (PrecipitationRenderer::wetness).
    void setWetness(f32 wetness) noexcept { m_wetness = wetness; }
    [[nodiscard]] f32 daylight() const noexcept { return m_daylight; }
    // The animation table of the block textures (BlockTextureData::animations,
    // one entry per texture key layer; 0 for still textures). Animated layers
    // show their frames in turn, 20 game ticks per second.
    void setTextureAnimations(std::span<const u32> animations);
    // The look of each texture layer (voxel::textureLook): its overlay and tints.
    void setTextureLooks(std::span<const u32> looks);
    // The color of each tint kind, 0xAABBGGRR in sRGB like texture colors.
    // Kind 0 stays white. Biomes replace this with a color per column (G3.7).
    void setTintColors(std::span<const u32> colors);
    // Graphics.Lighting.Brightness, for other renderers that light terrain alike.
    [[nodiscard]] f32 brightness() const { return m_brightness.get(); }
    // 1 with the classic face shading, 0 with Graphics.Lighting.NoShading.
    [[nodiscard]] f32 faceShading() const { return m_noShading.get() ? 0.0F : 1.0F; }
    // Multiplies World.Streaming.MaxUploadKBPerFrame, still capped by the
    // staging memory per frame. Streaming raises it in burst mode while the
    // loading screen shows (Section 11.6).
    void setUploadBudgetScale(f32 scale) noexcept;
    // The size of the target the chunks are drawn into, for the culling
    // frustum and the depth pyramid. 1920 x 1080 until set.
    void setViewSize(u32 width, u32 height) noexcept;

    // Starts a frame: new staging memory, releases ranges the GPU is done
    // with, and remembers the camera for culling and the translucent order.
    void beginFrame(const Camera& camera);

    enum class UploadResult : u8 {
        Uploaded,
        OverBudget,  // This frame's upload budget is used up; try again next frame.
        OutOfMemory, // The arena or the chunk slots are full.
    };
    // Makes `geometry` the chunk's mesh, replacing an older one. The quads
    // are copied, so the caller may reuse its mesh right away. A mesh without
    // quads draws nothing but still tells cave culling how the chunk
    // connects (a chunk of solid stone blocks the view); send those too.
    [[nodiscard]] UploadResult upload(const ChunkGeometry& geometry);
    // Forgets a chunk: stops drawing it, and cave culling treats its space as
    // open again. Returns false if the renderer did not know the chunk.
    bool remove(ChunkPos position);
    [[nodiscard]] bool isResident(ChunkPos position) const noexcept;

    // Copies this frame's uploads into the arena. Record in a Transfer pass
    // before the passes that draw chunks.
    void recordUploads(rhi::CommandList& commands);
    // Decides which face groups to draw this frame. Record after
    // recordUploads and before record, outside render passes.
    void recordCulling(rhi::CommandList& commands);
    // Draws one pass of every visible chunk. Record inside a raster pass
    // with the formats from the desc, after binding the frame constants.
    void record(rhi::CommandList& commands, ChunkPass pass);
    // Builds the depth pyramid for next frame's occlusion test from this
    // frame's depth (a Sampled texture of the view size). Record outside
    // render passes, after the opaque and cutout chunks were drawn.
    void recordOcclusionDepth(rhi::CommandList& commands, rhi::TextureHandle depth);

    [[nodiscard]] usize residentChunks() const noexcept { return m_chunks.size(); }
    [[nodiscard]] u64 residentQuads() const noexcept { return m_residentQuads; }
    [[nodiscard]] usize arenaUsedBytes() const noexcept;
    [[nodiscard]] usize arenaCapacityBytes() const noexcept { return m_arenaBytes; }
    // Candidate draws (non empty face groups) of a pass, before culling.
    [[nodiscard]] u32 drawCount(ChunkPass pass) const noexcept;
    // Where the pass's draws start in drawBuffer(), in draws. Each pass has
    // a region of its own with room to grow.
    [[nodiscard]] u32 drawFirst(ChunkPass pass) const noexcept {
        return m_drawFirst.at(static_cast<usize>(pass));
    }
    [[nodiscard]] usize uploadedBytesThisFrame() const noexcept { return m_stagingUsed; }
    [[nodiscard]] const ChunkCullingCounts& cullingCounts() const noexcept { return m_counts; }
    // One line for the debug overlay, for example
    // "Culling (GPU): 9120 of 61234 face groups drawn ...".
    [[nodiscard]] std::string cullingSummary() const;
    // The camera culling uses while Debug.FreezeCulling is on.
    [[nodiscard]] std::optional<Camera> frozenCullingCamera() const;
    [[nodiscard]] f32 viewAspect() const noexcept;
    [[nodiscard]] const CaveCuller& caveCuller() const noexcept { return m_caves; }
    // The buffers behind the draws, for tests and debug views.
    [[nodiscard]] rhi::BufferHandle arenaBuffer() const noexcept { return m_arena; }
    [[nodiscard]] rhi::BufferHandle metadataBuffer() const noexcept { return m_metadata; }
    [[nodiscard]] rhi::BufferHandle drawBuffer() const noexcept { return m_draws; }

    // One indirect draw as the GPU reads it (DrawElementsIndirectCommand).
    struct IndirectDraw {
        u32 indexCount;
        u32 instanceCount;
        u32 firstIndex;
        i32 baseVertex;
        u32 baseInstance; // The chunk's metadata slot (gl_BaseInstance).
    };

private:
    explicit ChunkRenderer(const ChunkRendererDesc& desc);

    // Passes whose draws are in no particular order (opaque, cutout).
    static constexpr usize kSortedPassCount = 3;
    static constexpr u32 kNoDraw = 0xFFFFFFFF;
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
    // Where this frame's draws come from.
    enum class CullPath : u8 {
        None, // recordCulling did not run: draw every candidate.
        Gpu,
        Cpu,
    };
    // One CpuRead buffer the culling counts are copied into, read once its
    // fence signals.
    struct Readback {
        rhi::BufferHandle buffer{};
        rhi::FenceHandle fence{};
        bool pending = false;
        u32 candidates = 0;
    };
    [[nodiscard]] Result<void> createCulling(const ChunkRendererDesc& desc);
    void registerStats();
    void release(const Resident& chunk);
    void writeMetadata(u32 slot, ChunkPos position, ChunkBounds bounds);
    [[nodiscard]] static IndirectDraw makeDraw(const Resident& chunk, usize group) noexcept;
    void addDraws(const Resident& chunk);
    void removeDraws(u32 slot);
    void markDrawsChanged(u32 first, u32 count);
    [[nodiscard]] bool relayoutDraws(const std::array<u32, kChunkPassCount>& capacities);
    void sortTranslucent();
    void flushDraws(rhi::CommandList& commands);
    void updateCaveBits(const Camera& cullCamera);
    void cullOnGpu(rhi::CommandList& commands, const Camera& cullCamera);
    void cullOnCpu(const Camera& cullCamera);
    void readCullingCounts();
    void publishStats();

    rhi::Device& m_device;
    stats::StatRegistry* m_stats;
    usize m_arenaBytes;
    u32 m_maxChunks;
    rhi::BufferHandle m_arena;
    rhi::BufferHandle m_metadata;
    rhi::BufferHandle m_indices;
    rhi::BufferHandle m_draws;      // Every candidate draw, in pass order.
    rhi::BufferHandle m_drawFaces;  // The face direction of each candidate.
    rhi::BufferHandle m_survivors;  // What the GPU culling kept.
    rhi::BufferHandle m_params;     // Per pass uniforms (AO strength), rewritten when a setting changes.
    rhi::BufferHandle m_animations; // One u32 per texture layer (setTextureAnimations).
    rhi::BufferHandle m_looks;      // One u32 per texture layer (setTextureLooks).
    std::array<Vec4, kBlockTintKinds> m_tints{};
    Vec4 m_lastParams{-1.0F};
    Vec4 m_lastSkyLight{-1.0F};
    usize m_drawCapacity = 0; // Indirect commands the draw buffers hold.
    std::array<rhi::PipelineHandle, kChunkPassCount> m_pipelines{};
    std::array<rhi::PipelineHandle, kChunkPassCount> m_wireframePipelines{};
    rhi::TextureHandle m_textures;
    rhi::SamplerHandle m_sampler;
    CpuWriteRing m_staging;
    std::span<std::byte> m_stagingMemory;
    usize m_stagingUsed = 0;
    f64 m_uploadBudgetScale = 1.0;
    std::vector<PendingCopy> m_pendingCopies;

    OffsetAllocator m_allocator;
    HashMap<ChunkPos, Resident> m_chunks;
    std::vector<u32> m_freeSlots;
    std::vector<DeferredFree> m_deferred;
    u64 m_frame = 0;
    u64 m_residentQuads = 0;
    ChunkPos m_cameraChunk{};
    Camera m_camera{};
    u32 m_viewWidth = 1920;
    u32 m_viewHeight = 1080;
    // The draw list (see the .cpp): the CPU copy of the draw buffer with the
    // face of each draw, one region per pass.
    std::vector<IndirectDraw> m_drawData;
    std::vector<u32> m_drawFaceData;
    std::array<u32, kChunkPassCount> m_drawFirst{};      // Region starts.
    std::array<u32, kChunkPassCount> m_regionCapacity{}; // Region sizes.
    std::array<u32, kChunkPassCount> m_drawCounts{};     // Draws per region now.
    std::array<u32, kChunkPassCount> m_gpuDrawCounts{};  // Draws per region on the GPU (last flush).
    // Per metadata slot: where its opaque and cutout draws are, per face.
    std::vector<std::array<u32, kSortedPassCount * kChunkFaceCount>> m_slotDraws;
    std::vector<u32> m_translucentSlots;           // Slots with translucent quads.
    std::vector<u32> m_translucentIndex;           // Per slot: its place in m_translucentSlots, or kNoDraw.
    std::vector<std::pair<u32, u32>> m_dirtyDraws; // [first, end) of changed draws since the flush.
    bool m_translucentDirty = true;
    bool m_layoutDirty = true; // The regions moved: the whole list goes to the GPU.
    CpuWriteRing m_drawStaging;
    std::span<std::byte> m_drawStagingMemory;
    usize m_drawStagingUsed = 0;
    std::vector<std::pair<i64, u32>> m_sortScratch; // (distance, slot) of translucent chunks.

    // Culling.
    CaveCuller m_caves;
    // Per metadata slot, for the CPU path and the cave bits.
    struct SlotInfo {
        ChunkPos position{};
        ChunkBounds bounds{};
    };
    std::vector<SlotInfo> m_slotInfo;
    std::vector<u32> m_caveBits; // Bit per slot: reached by the cave search.
    u64 m_caveGeneration = ~u64{0};
    bool m_caveBitsDirty = true;
    bool m_caveCullingWasOn = false;
    rhi::BufferHandle m_caveBitsBuffer;
    rhi::BufferHandle m_cullCounts; // Survivor counts per pass, then counts per reason.
    rhi::BufferHandle m_zeros;      // Copied over m_cullCounts before each culling dispatch.
    rhi::PipelineHandle m_cullPipeline;
    bool m_gpuCullingSupported = false;
    CpuWriteRing m_cullParams;
    CpuWriteRing m_cpuDraws; // Draws written by the CPU path, per frame in flight.
    std::array<u32, kChunkPassCount> m_cpuDrawCounts{};
    std::array<Readback, CpuWriteRing::kFrames> m_readbacks{};
    usize m_lastReadback = 0;
    bool m_readbackWritten = false;
    std::unique_ptr<HiZPyramid> m_hiZ;
    Camera m_occlusionCamera{}; // The camera that drew the depth in the pyramid.
    f32 m_occlusionAspect = 16.0F / 9.0F;
    std::optional<Camera> m_frozenCamera;
    f32 m_frozenAspect = 16.0F / 9.0F;
    CullPath m_cullPath = CullPath::None;
    u64 m_culledFrame = 0; // The latest frame recordCulling ran in.
    ChunkCullingCounts m_counts{};

    config::Setting<i32> m_uploadBudgetKb;
    config::Setting<f32> m_aoStrength;
    config::Setting<f32> m_brightness;
    config::Setting<bool> m_noShading;
    f32 m_daylight = 1.0F;
    Vec3 m_skyLight{1.0F};
    f32 m_wetness = 0.0F;
    config::Setting<bool> m_wireframe;
    config::Setting<bool> m_frustumCulling;
    config::Setting<f32> m_frustumPadding;
    config::Setting<bool> m_faceCulling;
    config::Setting<bool> m_caveCulling;
    config::Setting<bool> m_occlusionCulling;
    config::Setting<bool> m_gpuCulling;
    config::Setting<bool> m_freezeCulling;
    struct StatIds {
        stats::StatId chunks;
        stats::StatId quads;
        stats::StatId arenaBytes;
        stats::StatId bytesPerChunk;
        stats::StatId draws;
        stats::StatId uploadBytes;
        stats::StatId overBudget;
        stats::StatId outOfMemory;
        stats::StatId candidates;
        stats::StatId visible;
        stats::StatId frustum;
        stats::StatId direction;
        stats::StatId cave;
        stats::StatId occlusion;
        stats::StatId caveSearchMs;
        stats::StatId caveReached;
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
    // The average color of each layer (its 1 x 1 mip, RGBA8 as sRGB): what a
    // layer looks like from far away, for the far terrain's material table.
    std::vector<u32> averageColors;
};
[[nodiscard]] Result<BlockTextures> createPlaceholderBlockTextures(rhi::Device& device,
                                                                   std::span<const u32> colors);

} // namespace vxe::render
