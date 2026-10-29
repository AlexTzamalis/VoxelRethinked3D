#include "vxe/render/chunk_renderer.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/math/aabb.hpp>
#include <vxe/math/frustum.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <utility>

namespace vxe::render {
namespace {

using rhi::BufferUsage;

// A face group never holds more than 16 slices of 16 x 16 faces, so one
// shared index buffer for 4096 quads serves every draw, and its largest
// index (4095 * 4 + 3) still fits 16 bits.
constexpr u32 kMaxQuadsPerDraw = 16 * 16 * 16;
constexpr u32 kIndicesPerQuad = 6;

// One indirect draw as the GPU reads it (DrawElementsIndirectCommand).
struct IndirectDraw {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    i32 baseVertex;
    u32 baseInstance; // The chunk's metadata slot (gl_BaseInstance).
};
static_assert(sizeof(IndirectDraw) == 20);

// Per chunk shader data (ChunkInfo in render/chunk.vert and chunk_cull.comp).
struct ChunkInfo {
    glm::ivec4 position{0}; // xyz: chunk position, w: ChunkBounds::pack().
};
static_assert(sizeof(ChunkInfo) == 16);

struct ChunkParams {
    Vec4 params{1.0F, 0.0F, 0.0F, 0.0F}; // x: AO strength.
};

// The uniforms of render/chunk_cull.comp (std140; every member a 16 byte multiple).
struct CullParams {
    std::array<Vec4, kFrustumPlaneCount> planes{};
    glm::ivec4 cameraChunk{0};
    Vec4 cameraFraction{0.0F};
    Mat4 occlusionViewProjection{1.0F};
    glm::ivec4 occlusionChunk{0};
    Vec4 occlusionFraction{0.0F};
    glm::uvec4 occlusionSize{0};
    glm::uvec4 passFirst{0};
    glm::uvec4 flags{0};
    Vec4 tuning{0.0F};
};
static_assert(sizeof(CullParams) == (6 * 16) + (2 * 16) + 64 + (6 * 16));

// The counts buffer of the culling shader: survivors per pass, then the
// number of groups per reason (visible, frustum, direction, cave, occlusion).
constexpr usize kCountsBytes = 64;
constexpr usize kReasonOffset = 4;
constexpr u32 kCullGroupSize = 64;

// The test bits of CullParams::flags.x.
constexpr u32 kTestFrustum = 1U << 0U;
constexpr u32 kTestDirection = 1U << 1U;
constexpr u32 kTestCave = 1U << 2U;
constexpr u32 kTestOcclusion = 1U << 3U;

// Ranges freed in frame F may still be read by the GPU frames before it;
// they are reused once the staging ring proves those are done.
constexpr u64 kFramesInFlight = CpuWriteRing::kFrames;

// Occlusion compares against the depth a camera saw last frame; after a
// jump further than this (a teleport), the old depth says nothing useful.
constexpr f64 kMaxOcclusionCameraMove = 16.0;

} // namespace

ChunkRenderer::ChunkRenderer(const ChunkRendererDesc& desc)
    : m_device(*desc.device)
    , m_stats(desc.stats)
    , m_arenaBytes(desc.arenaBytes)
    , m_maxChunks(desc.maxChunks)
    , m_allocator(static_cast<u32>(desc.arenaBytes / kChunkQuadBytes), desc.maxChunks)
    , m_uploadBudgetKb(desc.settings->registerInt(
          {{.id = "World.Streaming.MaxUploadKBPerFrame",
            .displayName = "Upload budget per frame",
            .description = "Most chunk mesh data sent to the GPU in one frame, in KB. Lower avoids frame "
                           "spikes, higher fills the world in faster.",
            .visibility = config::Visibility::Advanced},
           std::min(4096, static_cast<i32>(desc.stagingBytesPerFrame / 1024)),
           64,
           std::max(64, static_cast<i32>(desc.stagingBytesPerFrame / 1024)),
           64}))
    , m_aoStrength(desc.settings->registerFloat({{.id = "Graphics.Terrain.AoStrength",
                                                  .displayName = "Ambient occlusion",
                                                  .description = "How dark corners between blocks get."},
                                                 1.0F,
                                                 0.0F,
                                                 1.0F,
                                                 0.05F}))
    , m_wireframe(desc.settings->registerBool({{.id = "Debug.Terrain.Wireframe",
                                                .displayName = "Terrain wireframe",
                                                .description = "Draws chunk quads as lines.",
                                                .visibility = config::Visibility::Developer},
                                               false}))
    , m_frustumCulling(desc.settings->registerBool({{.id = "Graphics.Culling.Frustum",
                                                     .displayName = "Frustum culling",
                                                     .description = "Skips chunks outside the view.",
                                                     .visibility = config::Visibility::Advanced},
                                                    true}))
    , m_frustumPadding(desc.settings->registerFloat(
          {{.id = "Graphics.Culling.FrustumPadding",
            .displayName = "Frustum padding",
            .description = "Grows every chunk's box by this many blocks for the view test, so chunks at "
                           "the screen edge appear a little early when turning fast.",
            .visibility = config::Visibility::Advanced},
           0.0F,
           0.0F,
           16.0F,
           0.5F}))
    , m_faceCulling(desc.settings->registerBool(
          {{.id = "Graphics.Terrain.FaceDirectionCulling",
            .displayName = "Face direction culling",
            .description = "Skips whole groups of faces that point away from the camera.",
            .visibility = config::Visibility::Advanced},
           true}))
    , m_caveCulling(desc.settings->registerBool(
          {{.id = "Graphics.Culling.CaveCulling",
            .displayName = "Cave culling",
            .description = "Skips chunks the camera cannot see through open space, such as caves seen "
                           "from the surface."},
           true}))
    , m_occlusionCulling(desc.settings->registerBool(
          {{.id = "Graphics.Culling.Occlusion",
            .displayName = "Occlusion culling",
            .description = "Skips chunks hidden behind what was drawn in the last frame (Hi-Z). Needs "
                           "GPU culling."},
           true}))
    , m_gpuCulling(desc.settings->registerBool(
          {{.id = "Graphics.Culling.Gpu",
            .displayName = "GPU culling",
            .description = "Culls on the GPU when the driver supports indirect count draws. Off culls on "
                           "the CPU, without occlusion culling.",
            .visibility = config::Visibility::Advanced},
           true}))
    , m_freezeCulling(desc.settings->registerBool(
          {{.id = "Debug.FreezeCulling",
            .displayName = "Freeze culling",
            .description = "Keeps culling from the camera where it was when this was turned on, so you "
                           "can fly around and look at what was culled.",
            .visibility = config::Visibility::Developer},
           false})) {
    m_chunks.reserve(desc.maxChunks);
    m_freeSlots.reserve(desc.maxChunks);
    for (u32 slot = desc.maxChunks; slot > 0; --slot) {
        m_freeSlots.push_back(slot - 1);
    }
    m_slotInfo.resize(desc.maxChunks);
    m_caveBits.assign((usize{desc.maxChunks} + 31) / 32, 0xFFFFFFFFU);
}

Result<std::unique_ptr<ChunkRenderer>> ChunkRenderer::create(const ChunkRendererDesc& desc) {
    VXE_ASSERT(desc.device != nullptr && desc.settings != nullptr,
               "ChunkRenderer needs a device and settings");
    VXE_ASSERT(desc.arenaBytes / kChunkQuadBytes <= 0x7FFFFFFFU / 4U,
               "Arena too large for 32 bit base vertices");
    std::unique_ptr<ChunkRenderer> renderer(new ChunkRenderer(desc));
    rhi::Device& device = *desc.device;
    ChunkRenderer& r = *renderer;

    // Buffers. The destructor cleans up whatever was created if a later
    // step fails, since handles start out invalid.
    auto arena = device.createBuffer(
        {.sizeBytes = desc.arenaBytes,
         .usage = BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc,
         .debugName = "Chunk vertex arena"});
    if (!arena) {
        return arena.error().withContext("Chunk renderer");
    }
    r.m_arena = arena.value();
    auto metadata = device.createBuffer(
        {.sizeBytes = usize{desc.maxChunks} * sizeof(ChunkInfo),
         .usage = BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc,
         .debugName = "Chunk metadata"});
    if (!metadata) {
        return metadata.error().withContext("Chunk renderer");
    }
    r.m_metadata = metadata.value();

    std::vector<u16> indices(usize{kMaxQuadsPerDraw} * kIndicesPerQuad);
    for (u32 quad = 0; quad < kMaxQuadsPerDraw; ++quad) {
        const auto base = static_cast<u16>(quad * 4);
        const std::array<u16, kIndicesPerQuad> pattern = {
            base, static_cast<u16>(base + 1), static_cast<u16>(base + 2),
            base, static_cast<u16>(base + 2), static_cast<u16>(base + 3)};
        std::ranges::copy(pattern, indices.begin() + (static_cast<std::ptrdiff_t>(quad) * kIndicesPerQuad));
    }
    auto indexBuffer = device.createBuffer({.sizeBytes = indices.size() * sizeof(u16),
                                            .usage = BufferUsage::Index | BufferUsage::TransferDst,
                                            .debugName = "Chunk quad indices"});
    if (!indexBuffer) {
        return indexBuffer.error().withContext("Chunk renderer");
    }
    r.m_indices = indexBuffer.value();
    VXE_TRY(device.updateBuffer(r.m_indices, 0, std::as_bytes(std::span(indices))));

    auto params = device.createBuffer({.sizeBytes = sizeof(ChunkParams),
                                       .usage = BufferUsage::Uniform | BufferUsage::TransferDst,
                                       .debugName = "Chunk parameters"});
    if (!params) {
        return params.error().withContext("Chunk renderer");
    }
    r.m_params = params.value();
    VXE_TRY(r.m_staging.create(device, desc.stagingBytesPerFrame, BufferUsage::TransferSrc,
                               "Chunk upload staging"));

    // One pipeline per pass, plus wireframe versions for Debug.Terrain.Wireframe.
    const auto vertex = rhi::createShaderFromFile(device, "render/chunk.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Chunk renderer");
    }
    const std::array<std::vector<rhi::ShaderDefine>, kChunkPassCount> defines = {
        std::vector<rhi::ShaderDefine>{},
        std::vector<rhi::ShaderDefine>{{.name = "VXE_CHUNK_CUTOUT"}},
        std::vector<rhi::ShaderDefine>{{.name = "VXE_CHUNK_TRANSLUCENT"}},
    };
    constexpr std::array<std::string_view, kChunkPassCount> kNames = {"Chunks opaque", "Chunks cutout",
                                                                      "Chunks translucent"};
    Result<void> built{};
    for (usize pass = 0; pass < kChunkPassCount && built; ++pass) {
        const auto fragment =
            rhi::createShaderFromFile(device, "render/chunk.frag", desc.shaders, defines.at(pass));
        if (!fragment) {
            built = fragment.error();
            break;
        }
        rhi::PipelineDesc pipeline{
            .vertex = vertex.value(), .fragment = fragment.value(), .debugName = kNames.at(pass)};
        pipeline.colorFormats[0] = desc.colorFormat;
        pipeline.colorCount = 1;
        pipeline.depthFormat = desc.depthFormat;
        if (static_cast<ChunkPass>(pass) == ChunkPass::Translucent) {
            // Blended over what is behind, seen from both sides (the water
            // surface from below), hiding nothing in the depth buffer.
            pipeline.blend[0] = rhi::BlendState::alphaBlend();
            pipeline.depth.writeEnabled = false;
            pipeline.raster.cullMode = rhi::CullMode::None;
        }
        auto solid = device.createPipeline(pipeline);
        pipeline.raster.polygonMode = rhi::PolygonMode::Line;
        auto lines = device.createPipeline(pipeline);
        device.destroy(fragment.value());
        if (solid) {
            r.m_pipelines.at(pass) = solid.value();
        }
        if (lines) {
            r.m_wireframePipelines.at(pass) = lines.value();
        }
        if (!solid || !lines) {
            built = solid ? lines.error() : solid.error();
        }
    }
    device.destroy(vertex.value());
    if (!built) {
        return built.error().withContext("Chunk renderer");
    }
    if (Result<void> culling = r.createCulling(desc); !culling) {
        return culling.error().withContext("Chunk renderer");
    }
    r.registerStats();
    return renderer;
}

// The buffers, pipeline and depth pyramid of the culling paths.
Result<void> ChunkRenderer::createCulling(const ChunkRendererDesc& desc) {
    auto counts = m_device.createBuffer({.sizeBytes = kCountsBytes,
                                         .usage = BufferUsage::Indirect | BufferUsage::Storage |
                                                  BufferUsage::TransferDst | BufferUsage::TransferSrc,
                                         .debugName = "Chunk culling counts"});
    if (!counts) {
        return counts.error();
    }
    m_cullCounts = counts.value();
    auto zeros = m_device.createBuffer({.sizeBytes = kCountsBytes,
                                        .usage = BufferUsage::TransferSrc | BufferUsage::TransferDst,
                                        .debugName = "Chunk culling zeros"});
    if (!zeros) {
        return zeros.error();
    }
    m_zeros = zeros.value();
    const std::array<std::byte, kCountsBytes> zeroBytes{};
    VXE_TRY(m_device.updateBuffer(m_zeros, 0, zeroBytes));
    auto caveBits = m_device.createBuffer({.sizeBytes = m_caveBits.size() * sizeof(u32),
                                           .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                           .debugName = "Chunk cave culling bits"});
    if (!caveBits) {
        return caveBits.error();
    }
    m_caveBitsBuffer = caveBits.value();
    VXE_TRY(m_device.updateBuffer(m_caveBitsBuffer, 0, std::as_bytes(std::span(m_caveBits))));
    VXE_TRY(
        m_cullParams.create(m_device, sizeof(CullParams), BufferUsage::Uniform, "Chunk culling parameters"));
    for (Readback& readback : m_readbacks) {
        auto buffer = m_device.createBuffer({.sizeBytes = kCountsBytes,
                                             .usage = BufferUsage::CpuRead | BufferUsage::TransferDst,
                                             .debugName = "Chunk culling readback"});
        if (!buffer) {
            return buffer.error();
        }
        readback.buffer = buffer.value();
    }
    const auto shader = rhi::createShaderFromFile(m_device, "render/chunk_cull.comp", desc.shaders);
    if (!shader) {
        return shader.error();
    }
    auto pipeline = m_device.createPipeline({.compute = shader.value(), .debugName = "Chunk culling"});
    m_device.destroy(shader.value());
    if (!pipeline) {
        return pipeline.error();
    }
    m_cullPipeline = pipeline.value();
    auto hiZ = HiZPyramid::create({.device = &m_device, .shaders = desc.shaders});
    if (!hiZ) {
        return hiZ.error();
    }
    m_hiZ = std::move(hiZ.value());
    m_gpuCullingSupported = m_device.capabilities().multiDrawIndirectCount;
    return {};
}

void ChunkRenderer::registerStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto add = [this](const stats::StatDesc& statDesc) {
        auto id = m_stats->registerStat(statDesc);
        return id ? id.value() : stats::StatId{};
    };
    using stats::StatType;
    using stats::StatUnit;
    m_statIds = {
        .chunks = add({.name = "Render.Chunks.Resident",
                       .description = "Chunks with a mesh on the GPU.",
                       .type = StatType::Gauge}),
        .quads = add({.name = "Render.Chunks.Quads",
                      .description = "Quads of all resident chunks.",
                      .type = StatType::Gauge}),
        .arenaBytes = add({.name = "Render.Chunks.ArenaBytes",
                           .description = "Vertex arena bytes in use.",
                           .type = StatType::Gauge,
                           .unit = StatUnit::Bytes}),
        .bytesPerChunk = add({.name = "Render.Chunks.BytesPerChunk",
                              .description = "Average GPU bytes per resident chunk (quads and metadata).",
                              .type = StatType::Gauge,
                              .unit = StatUnit::Bytes}),
        .draws =
            add({.name = "Render.Chunks.Draws",
                 .description = "Candidate indirect draws (face groups) over all passes, before culling.",
                 .type = StatType::Gauge}),
        .uploadBytes = add({.name = "Render.Chunks.UploadBytes",
                            .description = "Chunk mesh bytes uploaded this frame.",
                            .unit = StatUnit::Bytes}),
        .overBudget = add({.name = "Render.Chunks.UploadsOverBudget",
                           .description = "Uploads refused this frame because the budget was used."}),
        .outOfMemory =
            add({.name = "Render.Chunks.UploadsOutOfMemory",
                 .description = "Uploads refused because the arena or the chunk slots were full."}),
        .candidates = add({.name = "Render.Culling.FaceGroups",
                           .description = "Face groups the latest culling looked at.",
                           .type = StatType::Gauge}),
        .visible = add(
            {.name = "Render.Culling.Visible", .description = "Face groups drawn.", .type = StatType::Gauge}),
        .frustum = add({.name = "Render.Culling.Frustum",
                        .description = "Face groups culled because their chunk is outside the view.",
                        .type = StatType::Gauge}),
        .direction = add({.name = "Render.Culling.Direction",
                          .description = "Face groups culled because they point away from the camera.",
                          .type = StatType::Gauge}),
        .cave = add({.name = "Render.Culling.Cave",
                     .description = "Face groups culled because the cave search did not reach their chunk.",
                     .type = StatType::Gauge}),
        .occlusion = add({.name = "Render.Culling.Occlusion",
                          .description = "Face groups culled because last frame's depth hides their chunk.",
                          .type = StatType::Gauge}),
        .caveSearchMs = add({.name = "Render.Culling.CaveSearchMs",
                             .description = "Time of the latest cave search on the main thread.",
                             .type = StatType::Gauge,
                             .unit = StatUnit::Milliseconds}),
        .caveReached =
            add({.name = "Render.Culling.CaveReached",
                 .description = "Chunk cells (including open space) the latest cave search reached.",
                 .type = StatType::Gauge}),
    };
}

ChunkRenderer::~ChunkRenderer() {
    m_staging.destroy(m_device);
    m_cullParams.destroy(m_device);
    m_cpuDraws.destroy(m_device);
    m_hiZ.reset();
    for (const rhi::PipelineHandle pipeline : m_pipelines) {
        if (pipeline.isValid()) {
            m_device.destroy(pipeline);
        }
    }
    for (const rhi::PipelineHandle pipeline : m_wireframePipelines) {
        if (pipeline.isValid()) {
            m_device.destroy(pipeline);
        }
    }
    if (m_cullPipeline.isValid()) {
        m_device.destroy(m_cullPipeline);
    }
    for (const Readback& readback : m_readbacks) {
        if (readback.fence.isValid()) {
            m_device.destroy(readback.fence);
        }
        if (readback.buffer.isValid()) {
            m_device.destroy(readback.buffer);
        }
    }
    for (const rhi::BufferHandle buffer : {m_arena, m_metadata, m_indices, m_draws, m_drawFaces, m_survivors,
                                           m_params, m_caveBitsBuffer, m_cullCounts, m_zeros}) {
        if (buffer.isValid()) {
            m_device.destroy(buffer);
        }
    }
}

void ChunkRenderer::setTextures(rhi::TextureHandle textures, rhi::SamplerHandle sampler) noexcept {
    m_textures = textures;
    m_sampler = sampler;
}

void ChunkRenderer::setUploadBudgetScale(f32 scale) noexcept {
    m_uploadBudgetScale = std::max(static_cast<f64>(scale), 0.0);
}

void ChunkRenderer::setViewSize(u32 width, u32 height) noexcept {
    m_viewWidth = std::max(width, 1U);
    m_viewHeight = std::max(height, 1U);
}

f32 ChunkRenderer::viewAspect() const noexcept {
    return static_cast<f32>(m_viewWidth) / static_cast<f32>(m_viewHeight);
}

void ChunkRenderer::beginFrame(const Camera& camera) {
    VXE_PROFILE_FUNCTION();
    ++m_frame;
    m_stagingMemory = m_staging.next(m_device);
    m_stagingUsed = 0;
    m_pendingCopies.clear();
    m_cullPath = CullPath::None;
    // next() waited for the GPU to finish the frame kFramesInFlight ago, so
    // ranges freed before then are no longer read.
    std::erase_if(m_deferred, [this](const DeferredFree& entry) {
        if (entry.frame + kFramesInFlight > m_frame) {
            return false;
        }
        m_allocator.free(entry.range);
        if (entry.slot != OffsetAllocator::kNoSpace) {
            m_freeSlots.push_back(entry.slot);
        }
        return true;
    });
    m_camera = camera;
    const ChunkPos cameraChunk = camera.renderPosition().chunk;
    if (cameraChunk != m_cameraChunk) {
        m_cameraChunk = cameraChunk;
        m_drawsDirty = true; // The translucent order depends on it.
    }
    if (m_freezeCulling.get()) {
        if (!m_frozenCamera) {
            m_frozenCamera = camera;
            m_frozenAspect = viewAspect();
        }
    } else {
        m_frozenCamera.reset();
    }
    const ChunkParams params{.params = Vec4(m_aoStrength.get(), 0.0F, 0.0F, 0.0F)};
    if (params.params != m_lastParams) {
        m_lastParams = params.params;
        static_cast<void>(m_device.updateBuffer(m_params, 0, std::as_bytes(std::span(&params, 1))));
    }
    readCullingCounts();
    if (m_occlusionCulling.get() && !m_frozenCamera) {
        static_cast<void>(m_hiZ->ensureSize(m_viewWidth, m_viewHeight));
    }
}

void ChunkRenderer::writeMetadata(u32 slot, ChunkPos position, ChunkBounds bounds) {
    const ChunkInfo info{
        .position = glm::ivec4(position.x, position.y, position.z, std::bit_cast<i32>(bounds.pack()))};
    static_cast<void>(m_device.updateBuffer(m_metadata, usize{slot} * sizeof(ChunkInfo),
                                            std::as_bytes(std::span(&info, 1))));
    m_slotInfo[slot] = {.position = position, .bounds = bounds};
}

ChunkRenderer::UploadResult ChunkRenderer::upload(const ChunkGeometry& geometry) {
    VXE_ASSERT(!m_stagingMemory.empty(), "Call beginFrame before uploading");
    VXE_ASSERT(geometry.quads.size() % kChunkQuadBytes == 0, "Quads are 8 bytes each");
    const usize bytes = geometry.quads.size();
    if (bytes == 0) {
        // Nothing to draw, but the chunk may still block the view.
        if (const auto it = m_chunks.find(geometry.position); it != m_chunks.end()) {
            release(it->second);
            m_chunks.erase(it);
            m_drawsDirty = true;
            m_caveBitsDirty = true;
        }
        m_caves.setChunk(geometry.position, geometry.connectivity);
        return UploadResult::Uploaded;
    }
    // The budget may be passed by one chunk when nothing was uploaded yet
    // this frame; otherwise a chunk bigger than the budget would never fit.
    const auto scaled = static_cast<f64>(std::max(m_uploadBudgetKb.get(), 1)) * 1024.0 * m_uploadBudgetScale;
    const usize budget = std::min(static_cast<usize>(scaled), m_stagingMemory.size());
    if (m_stagingUsed + bytes > budget && (m_stagingUsed > 0 || bytes > m_stagingMemory.size())) {
        if (m_stats != nullptr && m_statIds.overBudget.isValid()) {
            m_stats->add(m_statIds.overBudget);
        }
        return UploadResult::OverBudget;
    }
    const auto quads = static_cast<u32>(bytes / kChunkQuadBytes);
    auto existing = m_chunks.find(geometry.position);
    const bool replacing = existing != m_chunks.end();
    const OffsetAllocator::Allocation range = m_allocator.allocate(quads);
    if (!range.isValid() || (!replacing && m_freeSlots.empty())) {
        m_allocator.free(range);
        if (m_stats != nullptr && m_statIds.outOfMemory.isValid()) {
            m_stats->add(m_statIds.outOfMemory);
        }
        return UploadResult::OutOfMemory;
    }

    std::memcpy(m_stagingMemory.data() + m_stagingUsed, geometry.quads.data(), bytes);
    m_pendingCopies.push_back({.stagingOffset = m_stagingUsed,
                               .arenaOffset = usize{range.offset} * kChunkQuadBytes,
                               .bytes = bytes});
    m_stagingUsed += bytes;

    if (replacing) {
        // The old range may still be drawn by frames in flight; the slot
        // keeps its position, so it is reused right away.
        Resident& chunk = existing->second;
        m_residentQuads -= m_allocator.sizeOf(chunk.range);
        m_deferred.push_back({.frame = m_frame, .range = chunk.range, .slot = OffsetAllocator::kNoSpace});
        chunk.range = range;
        chunk.groupBegin = geometry.groupBegin;
        writeMetadata(chunk.slot, geometry.position, geometry.bounds);
    } else {
        const u32 slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        writeMetadata(slot, geometry.position, geometry.bounds);
        m_chunks.emplace(geometry.position, Resident{.position = geometry.position,
                                                     .range = range,
                                                     .slot = slot,
                                                     .groupBegin = geometry.groupBegin});
        m_caveBitsDirty = true;
    }
    m_caves.setChunk(geometry.position, geometry.connectivity);
    m_residentQuads += quads;
    m_drawsDirty = true;
    return UploadResult::Uploaded;
}

bool ChunkRenderer::remove(ChunkPos position) {
    m_caves.removeChunk(position);
    const auto it = m_chunks.find(position);
    if (it == m_chunks.end()) {
        return false;
    }
    release(it->second);
    m_chunks.erase(it);
    m_drawsDirty = true;
    m_caveBitsDirty = true;
    return true;
}

void ChunkRenderer::release(const Resident& chunk) {
    m_residentQuads -= m_allocator.sizeOf(chunk.range);
    m_deferred.push_back({.frame = m_frame, .range = chunk.range, .slot = chunk.slot});
}

bool ChunkRenderer::isResident(ChunkPos position) const noexcept {
    return m_chunks.contains(position);
}

usize ChunkRenderer::arenaUsedBytes() const noexcept {
    return usize{m_allocator.capacity() - m_allocator.freeSize()} * kChunkQuadBytes;
}

u32 ChunkRenderer::drawCount(ChunkPass pass) const noexcept {
    return m_drawCounts.at(static_cast<usize>(pass));
}

void ChunkRenderer::recordUploads(rhi::CommandList& commands) {
    VXE_PROFILE_FUNCTION();
    for (const PendingCopy& copy : m_pendingCopies) {
        commands.copyBuffer(m_staging.current(), copy.stagingOffset, m_arena, copy.arenaOffset, copy.bytes);
    }
    if (!m_pendingCopies.empty()) {
        commands.resourceBarrier(rhi::BarrierScope::StorageData);
    }
    if (m_drawsDirty) {
        rebuildDraws();
    }
    publishStats();
}

void ChunkRenderer::rebuildDraws() {
    VXE_PROFILE_FUNCTION();
    m_drawsDirty = false;
    // Culling results this frame index the old list.
    m_cullPath = CullPath::None;
    m_drawData.clear();
    m_drawFaceData.clear();
    std::array<std::vector<IndirectDraw>, kChunkPassCount> perPass;
    std::array<std::vector<u32>, kChunkPassCount> perPassFaces;
    const auto addDraws = [&](const Resident& chunk, usize pass) {
        for (u32 face = 0; face < kChunkFaceCount; ++face) {
            const usize group = (pass * kChunkFaceCount) + face;
            const u32 first = chunk.groupBegin.at(group);
            const u32 count = std::min(chunk.groupBegin.at(group + 1) - first, kMaxQuadsPerDraw);
            if (count == 0) {
                continue;
            }
            perPass.at(pass).push_back({.indexCount = count * kIndicesPerQuad,
                                        .instanceCount = 1,
                                        .firstIndex = 0,
                                        .baseVertex = static_cast<i32>((chunk.range.offset + first) * 4),
                                        .baseInstance = chunk.slot});
            perPassFaces.at(pass).push_back(face);
        }
    };
    m_sortScratch.clear();
    for (const auto& entry : m_chunks) {
        const Resident& chunk = entry.second;
        addDraws(chunk, static_cast<usize>(ChunkPass::Opaque));
        addDraws(chunk, static_cast<usize>(ChunkPass::Cutout));
        const usize translucent = static_cast<usize>(ChunkPass::Translucent) * kChunkFaceCount;
        if (chunk.groupBegin.at(translucent + kChunkFaceCount) > chunk.groupBegin.at(translucent)) {
            const i64 dx = static_cast<i64>(chunk.position.x) - m_cameraChunk.x;
            const i64 dy = static_cast<i64>(chunk.position.y) - m_cameraChunk.y;
            const i64 dz = static_cast<i64>(chunk.position.z) - m_cameraChunk.z;
            m_sortScratch.emplace_back((dx * dx) + (dy * dy) + (dz * dz), &chunk);
        }
    }
    // Blended chunks far to near, so nearer water and glass blend over farther ones.
    std::ranges::sort(m_sortScratch, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [distance, chunk] : m_sortScratch) {
        addDraws(*chunk, static_cast<usize>(ChunkPass::Translucent));
    }

    usize total = 0;
    for (usize pass = 0; pass < kChunkPassCount; ++pass) {
        m_drawFirst.at(pass) = static_cast<u32>(total);
        m_drawCounts.at(pass) = static_cast<u32>(perPass.at(pass).size());
        total += perPass.at(pass).size();
    }
    if (!growDrawBuffers(total)) {
        m_drawCounts.fill(0);
        return;
    }
    for (usize pass = 0; pass < kChunkPassCount; ++pass) {
        const auto bytes = std::as_bytes(std::span(perPass.at(pass)));
        m_drawData.insert(m_drawData.end(), bytes.begin(), bytes.end());
        m_drawFaceData.insert(m_drawFaceData.end(), perPassFaces.at(pass).begin(),
                              perPassFaces.at(pass).end());
    }
    if (!m_drawData.empty()) {
        static_cast<void>(m_device.updateBuffer(m_draws, 0, m_drawData));
        static_cast<void>(m_device.updateBuffer(m_drawFaces, 0, std::as_bytes(std::span(m_drawFaceData))));
    }
}

// Grows the candidate, face and survivor buffers to the next power of two
// that holds `total` draws; the device destroys the old ones once the GPU is
// done with them.
bool ChunkRenderer::growDrawBuffers(usize total) {
    if (total <= m_drawCapacity) {
        return true;
    }
    usize capacity = std::max<usize>(m_drawCapacity, 1024);
    while (capacity < total) {
        capacity *= 2;
    }
    for (rhi::BufferHandle* buffer : {&m_draws, &m_drawFaces, &m_survivors}) {
        if (buffer->isValid()) {
            m_device.destroy(*buffer);
            *buffer = {};
        }
    }
    m_drawCapacity = 0;
    auto draws = m_device.createBuffer({.sizeBytes = capacity * sizeof(IndirectDraw),
                                        .usage = BufferUsage::Indirect | BufferUsage::Storage |
                                                 BufferUsage::TransferDst | BufferUsage::TransferSrc,
                                        .debugName = "Chunk draws"});
    auto faces = m_device.createBuffer({.sizeBytes = capacity * sizeof(u32),
                                        .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                        .debugName = "Chunk draw faces"});
    auto survivors = m_device.createBuffer(
        {.sizeBytes = capacity * sizeof(IndirectDraw),
         .usage = BufferUsage::Indirect | BufferUsage::Storage | BufferUsage::TransferSrc,
         .debugName = "Chunk draws after culling"});
    if (draws) {
        m_draws = draws.value();
    }
    if (faces) {
        m_drawFaces = faces.value();
    }
    if (survivors) {
        m_survivors = survivors.value();
    }
    if (!draws || !faces || !survivors) {
        return false;
    }
    m_drawCapacity = capacity;
    return true;
}

void ChunkRenderer::updateCaveBits(const Camera& cullCamera) {
    m_caves.update(cullCamera.renderPosition().chunk);
    if (m_caves.generation() == m_caveGeneration && !m_caveBitsDirty) {
        return;
    }
    VXE_PROFILE_SCOPE("Cave culling bits");
    m_caveGeneration = m_caves.generation();
    m_caveBitsDirty = false;
    std::ranges::fill(m_caveBits, 0U);
    for (const auto& [position, chunk] : m_chunks) {
        if (m_caves.isVisible(position)) {
            m_caveBits[chunk.slot >> 5U] |= 1U << (chunk.slot & 31U);
        }
    }
    static_cast<void>(m_device.updateBuffer(m_caveBitsBuffer, 0, std::as_bytes(std::span(m_caveBits))));
}

void ChunkRenderer::recordCulling(rhi::CommandList& commands) {
    VXE_PROFILE_FUNCTION();
    if (m_drawsDirty) {
        rebuildDraws();
    }
    m_culledFrame = m_frame;
    const Camera& cullCamera = m_frozenCamera ? *m_frozenCamera : m_camera;
    if (m_caveCulling.get()) {
        updateCaveBits(cullCamera);
    }
    if (m_drawData.empty()) {
        m_cullPath = CullPath::Cpu;
        m_cpuDrawCounts.fill(0);
        m_counts = {.onGpu = false};
        return;
    }
    if (m_gpuCullingSupported && m_gpuCulling.get()) {
        cullOnGpu(commands, cullCamera);
    } else {
        cullOnCpu(cullCamera);
    }
}

void ChunkRenderer::cullOnGpu(rhi::CommandList& commands, const Camera& cullCamera) {
    const auto total = static_cast<u32>(m_drawData.size() / sizeof(IndirectDraw));
    const f32 aspect = m_frozenCamera ? m_frozenAspect : viewAspect();
    const Frustum frustum = Frustum::fromViewProjection(cullCamera.viewProjection(aspect));
    const CameraPosition position = cullCamera.renderPosition();
    CullParams params{};
    for (usize plane = 0; plane < kFrustumPlaneCount; ++plane) {
        const Plane& p = frustum.plane(static_cast<FrustumPlane>(plane));
        params.planes.at(plane) = Vec4(p.normal, p.distance);
    }
    params.cameraChunk = glm::ivec4(position.chunk.x, position.chunk.y, position.chunk.z, 0);
    params.cameraFraction = Vec4(position.fraction, 0.0F);
    u32 tests = (m_frustumCulling.get() ? kTestFrustum : 0U) | (m_faceCulling.get() ? kTestDirection : 0U) |
                (m_caveCulling.get() ? kTestCave : 0U);
    const bool occlusionUsable =
        m_occlusionCulling.get() && m_hiZ->hasData() &&
        glm::distance(m_occlusionCamera.position, cullCamera.position) <= kMaxOcclusionCameraMove;
    if (occlusionUsable) {
        tests |= kTestOcclusion;
        const CameraPosition occluder = m_occlusionCamera.renderPosition();
        params.occlusionViewProjection = m_occlusionCamera.viewProjection(m_occlusionAspect);
        params.occlusionChunk = glm::ivec4(occluder.chunk.x, occluder.chunk.y, occluder.chunk.z, 0);
        params.occlusionFraction = Vec4(occluder.fraction, 0.0F);
        params.occlusionSize = glm::uvec4(m_hiZ->depthWidth(), m_hiZ->depthHeight(), m_hiZ->levels(), 0);
    }
    params.passFirst = glm::uvec4(m_drawFirst[0], m_drawFirst[1], m_drawFirst[2], total);
    params.flags = glm::uvec4(tests, 0, 0, 0);
    params.tuning = Vec4(m_frustumPadding.get(), m_occlusionCamera.nearPlane, 0.0F, 0.0F);
    const std::span<std::byte> memory = m_cullParams.next(m_device);
    std::memcpy(memory.data(), &params, sizeof(params));

    commands.copyBuffer(m_zeros, 0, m_cullCounts, 0, kCountsBytes);
    commands.bindPipeline(m_cullPipeline);
    commands.bindUniformBuffer(1, 0, m_cullParams.current(), 0, sizeof(CullParams));
    commands.bindStorageBuffer(2, 0, m_draws, 0, usize{total} * sizeof(IndirectDraw));
    commands.bindStorageBuffer(2, 1, m_drawFaces, 0, usize{total} * sizeof(u32));
    commands.bindStorageBuffer(2, 2, m_metadata, 0, usize{m_maxChunks} * sizeof(ChunkInfo));
    commands.bindStorageBuffer(2, 3, m_caveBitsBuffer, 0, m_caveBits.size() * sizeof(u32));
    commands.bindStorageBuffer(2, 4, m_survivors, 0, usize{total} * sizeof(IndirectDraw));
    commands.bindStorageBuffer(2, 5, m_cullCounts, 0, kCountsBytes);
    commands.bindTexture(2, 6, m_hiZ->texture(), m_hiZ->sampler());
    commands.dispatch((total + kCullGroupSize - 1) / kCullGroupSize);
    commands.resourceBarrier(rhi::BarrierScope::IndirectCommands | rhi::BarrierScope::StorageData |
                             rhi::BarrierScope::TransferData);
    // The counts reach the overlay a few frames later, without stalling.
    for (usize i = 0; i < m_readbacks.size(); ++i) {
        Readback& readback = m_readbacks.at(i);
        if (!readback.pending) {
            commands.copyBuffer(m_cullCounts, 0, readback.buffer, 0, kCountsBytes);
            readback.pending = true;
            readback.candidates = total;
            m_lastReadback = i;
            m_readbackWritten = true;
            break;
        }
    }
    m_cullPath = CullPath::Gpu;
}

// The same tests as render/chunk_cull.comp except occlusion, for drivers
// without indirect count draws and for comparing the two paths.
void ChunkRenderer::cullOnCpu(const Camera& cullCamera) {
    VXE_PROFILE_FUNCTION();
    const usize needed = m_drawCapacity * sizeof(IndirectDraw);
    if (m_cpuDraws.bytesPerFrame() < needed) {
        m_cpuDraws.destroy(m_device);
        if (!m_cpuDraws.create(m_device, needed, BufferUsage::Indirect, "Chunk draws (CPU culling)")) {
            m_cullPath = CullPath::None;
            return;
        }
    }
    const std::span<std::byte> memory = m_cpuDraws.next(m_device);
    const f32 aspect = m_frozenCamera ? m_frozenAspect : viewAspect();
    const Frustum frustum = Frustum::fromViewProjection(cullCamera.viewProjection(aspect));
    const CameraPosition camera = cullCamera.renderPosition();
    const bool frustumOn = m_frustumCulling.get();
    const bool faceOn = m_faceCulling.get();
    const bool caveOn = m_caveCulling.get();
    const f32 padding = m_frustumPadding.get();
    ChunkCullingCounts counts{.onGpu = false};
    // A chunk's face groups sit next to each other in the list, so the box
    // and the frustum test are worked out once per chunk, not per group.
    u32 boxSlot = OffsetAllocator::kNoSpace;
    Vec3 boxMin(0.0F);
    Vec3 boxMax(0.0F);
    bool inFrustum = true;
    for (usize pass = 0; pass < kChunkPassCount; ++pass) {
        const u32 first = m_drawFirst.at(pass);
        u32 kept = 0;
        for (u32 i = first; i < first + m_drawCounts.at(pass); ++i) {
            IndirectDraw draw{};
            std::memcpy(&draw, m_drawData.data() + (usize{i} * sizeof(IndirectDraw)), sizeof(draw));
            if (draw.baseInstance != boxSlot) {
                boxSlot = draw.baseInstance;
                const SlotInfo& info = m_slotInfo[boxSlot];
                const Vec3 origin = relativeToCamera(info.position, camera);
                boxMin = origin + Vec3(info.bounds.min[0], info.bounds.min[1], info.bounds.min[2]);
                boxMax = origin + Vec3(info.bounds.max[0], info.bounds.max[1], info.bounds.max[2]);
                inFrustum = !frustumOn || frustum.intersects(Aabb{.min = boxMin - Vec3(padding),
                                                                  .max = boxMax + Vec3(padding)});
            }
            ++counts.candidates;
            if (!inFrustum) {
                ++counts.frustum;
                continue;
            }
            if (faceOn && pass != static_cast<usize>(ChunkPass::Translucent) &&
                !faceGroupFacesCamera(m_drawFaceData[i], boxMin, boxMax)) {
                ++counts.direction;
                continue;
            }
            if (caveOn && (m_caveBits[draw.baseInstance >> 5U] & (1U << (draw.baseInstance & 31U))) == 0) {
                ++counts.cave;
                continue;
            }
            // Survivors keep their order, so translucent stays far to near.
            std::memcpy(memory.data() + (usize{first + kept} * sizeof(IndirectDraw)), &draw, sizeof(draw));
            ++kept;
        }
        m_cpuDrawCounts.at(pass) = kept;
        counts.visible += kept;
    }
    m_counts = counts;
    m_cullPath = CullPath::Cpu;
}

// Reads GPU culling counts whose copy has finished. The fence for last
// frame's copy goes in now, after that frame was submitted.
void ChunkRenderer::readCullingCounts() {
    if (m_readbackWritten) {
        m_readbacks.at(m_lastReadback).fence = m_device.insertFence();
        m_readbackWritten = false;
    }
    for (Readback& readback : m_readbacks) {
        if (!readback.pending || !readback.fence.isValid() || !m_device.isFenceSignaled(readback.fence)) {
            continue;
        }
        m_device.destroy(readback.fence);
        readback.fence = {};
        readback.pending = false;
        const std::span<std::byte> mapped = m_device.mappedRange(readback.buffer);
        std::array<u32, kCountsBytes / sizeof(u32)> values{};
        std::memcpy(values.data(), mapped.data(), kCountsBytes);
        m_counts = {.candidates = readback.candidates,
                    .visible = values[kReasonOffset],
                    .frustum = values[kReasonOffset + 1],
                    .direction = values[kReasonOffset + 2],
                    .cave = values[kReasonOffset + 3],
                    .occlusion = values[kReasonOffset + 4],
                    .onGpu = true};
    }
}

void ChunkRenderer::record(rhi::CommandList& commands, ChunkPass pass) {
    if (m_drawsDirty) {
        rebuildDraws();
    }
    const auto index = static_cast<usize>(pass);
    const u32 count = m_drawCounts.at(index);
    if (count == 0 || !m_textures.isValid() ||
        (m_cullPath == CullPath::Cpu && m_cpuDrawCounts.at(index) == 0)) {
        return;
    }
    commands.bindPipeline(m_wireframe.get() ? m_wireframePipelines.at(index) : m_pipelines.at(index));
    commands.bindUniformBuffer(1, 0, m_params, 0, sizeof(ChunkParams));
    commands.bindStorageBuffer(2, 0, m_arena, 0, m_arenaBytes);
    commands.bindStorageBuffer(2, 1, m_metadata, 0, usize{m_maxChunks} * sizeof(ChunkInfo));
    commands.bindTexture(2, 2, m_textures, m_sampler);
    commands.bindIndexBuffer(m_indices, rhi::IndexType::Uint16);
    const usize offset = usize{m_drawFirst.at(index)} * sizeof(IndirectDraw);
    switch (m_cullPath) {
    case CullPath::Gpu:
        if (pass == ChunkPass::Translucent) {
            commands.multiDrawIndexedIndirect(m_survivors, offset, count);
        } else {
            commands.multiDrawIndexedIndirectCount(m_survivors, offset, m_cullCounts, index * sizeof(u32),
                                                   count);
        }
        break;
    case CullPath::Cpu:
        commands.multiDrawIndexedIndirect(m_cpuDraws.current(), offset, m_cpuDrawCounts.at(index));
        break;
    case CullPath::None:
        commands.multiDrawIndexedIndirect(m_draws, offset, count);
        break;
    }
}

void ChunkRenderer::recordOcclusionDepth(rhi::CommandList& commands, rhi::TextureHandle depth) {
    // A frozen culling camera keeps the pyramid it saw.
    if (!m_occlusionCulling.get() || m_frozenCamera || !(m_gpuCullingSupported && m_gpuCulling.get())) {
        return;
    }
    if (m_hiZ->depthWidth() != m_viewWidth || m_hiZ->depthHeight() != m_viewHeight) {
        return; // The view size changed after beginFrame; the next frame resizes.
    }
    m_hiZ->record(commands, depth);
    m_occlusionCamera = m_camera;
    m_occlusionAspect = viewAspect();
}

std::optional<Camera> ChunkRenderer::frozenCullingCamera() const {
    return m_frozenCamera;
}

std::string ChunkRenderer::cullingSummary() const {
    if (m_culledFrame + kFramesInFlight < m_frame) {
        return "Culling off (the Visibility pass did not run): every face group drawn";
    }
    const ChunkCullingCounts& c = m_counts;
    return std::format(
        "Culling ({}{}): {} of {} face groups drawn   frustum {}  direction {}  cave {}  occlusion {}",
        c.onGpu ? "GPU" : "CPU", m_frozenCamera ? ", frozen" : "", c.visible, c.candidates, c.frustum,
        c.direction, c.cave, c.occlusion);
}

void ChunkRenderer::publishStats() {
    if (m_stats == nullptr) {
        return;
    }
    const auto set = [this](stats::StatId id, f64 value) {
        if (id.isValid()) {
            m_stats->set(id, value);
        }
    };
    const auto chunks = static_cast<f64>(m_chunks.size());
    set(m_statIds.chunks, chunks);
    set(m_statIds.quads, static_cast<f64>(m_residentQuads));
    set(m_statIds.arenaBytes, static_cast<f64>(arenaUsedBytes()));
    set(m_statIds.bytesPerChunk,
        m_chunks.empty()
            ? 0.0
            : (static_cast<f64>(m_residentQuads * kChunkQuadBytes) / chunks) + sizeof(ChunkInfo));
    set(m_statIds.draws, static_cast<f64>(m_drawCounts[0] + m_drawCounts[1] + m_drawCounts[2]));
    if (m_statIds.uploadBytes.isValid()) {
        m_stats->add(m_statIds.uploadBytes, static_cast<i64>(m_stagingUsed));
    }
    set(m_statIds.candidates, m_counts.candidates);
    set(m_statIds.visible, m_counts.visible);
    set(m_statIds.frustum, m_counts.frustum);
    set(m_statIds.direction, m_counts.direction);
    set(m_statIds.cave, m_counts.cave);
    set(m_statIds.occlusion, m_counts.occlusion);
    set(m_statIds.caveSearchMs, m_caves.lastSearchMilliseconds());
    set(m_statIds.caveReached, static_cast<f64>(m_caves.reachedCells()));
}

Result<BlockTextures> createPlaceholderBlockTextures(rhi::Device& device, std::span<const u32> colors) {
    constexpr u32 kSize = 16;
    constexpr u32 kMips = 5; // 16, 8, 4, 2, 1.
    const auto layers = static_cast<u32>(std::max<usize>(colors.size(), 1));
    auto texture = device.createTexture({.type = rhi::TextureType::Texture2DArray,
                                         .format = rhi::Format::RGBA8Srgb,
                                         .width = kSize,
                                         .height = kSize,
                                         .layers = layers,
                                         .mipLevels = kMips,
                                         .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                         .debugName = "Placeholder block textures"});
    if (!texture) {
        return texture.error();
    }
    std::vector<u32> pixels(usize{kSize} * kSize);
    std::vector<u32> half;
    for (u32 layer = 0; layer < layers; ++layer) {
        const u32 color = colors.empty() ? 0xFFFF00FFU : colors[layer];
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                // A fixed hash per pixel varies the brightness by up to 12%,
                // so large faces read as blocks instead of flat paint.
                u32 h = (x * 73856093U) ^ (y * 19349663U) ^ (layer * 83492791U);
                h ^= h >> 13U;
                h *= 0x5BD1E995U;
                h ^= h >> 15U;
                const u32 scale = 225 + (h % 31); // 225 to 255 of 255.
                u32 pixel = color & 0xFF000000U;
                for (u32 channel = 0; channel < 3; ++channel) {
                    const u32 value = (color >> (channel * 8U)) & 0xFFU;
                    pixel |= ((value * scale) / 255U) << (channel * 8U);
                }
                pixels[(y * kSize) + x] = pixel;
            }
        }
        // Mipmaps by averaging 2 x 2 texels.
        std::vector<u32> level = pixels;
        u32 size = kSize;
        for (u32 mip = 0; mip < kMips; ++mip) {
            VXE_TRY(device.uploadTexture(
                texture.value(), {.mipLevel = mip, .layer = layer, .data = std::as_bytes(std::span(level))}));
            if (size == 1) {
                break;
            }
            const u32 next = size / 2;
            half.assign(usize{next} * next, 0);
            for (u32 y = 0; y < next; ++y) {
                for (u32 x = 0; x < next; ++x) {
                    u32 averaged = 0;
                    for (u32 channel = 0; channel < 4; ++channel) {
                        const u32 shift = channel * 8U;
                        u32 sum = 0;
                        for (const u32 texel :
                             {level[(2 * y * size) + (2 * x)], level[(2 * y * size) + (2 * x) + 1],
                              level[(((2 * y) + 1) * size) + (2 * x)],
                              level[(((2 * y) + 1) * size) + (2 * x) + 1]}) {
                            sum += (texel >> shift) & 0xFFU;
                        }
                        averaged |= (sum / 4) << shift;
                    }
                    half[(y * next) + x] = averaged;
                }
            }
            level.swap(half);
            size = next;
        }
    }
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::Linear,
                                         .debugName = "Block textures"});
    if (!sampler) {
        device.destroy(texture.value());
        return sampler.error();
    }
    return BlockTextures{.texture = texture.value(), .sampler = sampler.value()};
}

} // namespace vxe::render
