#include "vxe/render/chunk_renderer.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/math/aabb.hpp>
#include <vxe/math/frustum.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
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

using IndirectDraw = ChunkRenderer::IndirectDraw;

// The layout must match ShadowParams in chunk.frag (std140).
struct ShadowParams {
    std::array<Mat4, kShadowMaxCascades> matrices{};
    Vec4 splits{0.0F};     // Distance where each cascade ends.
    Vec4 depthRange{1.0F}; // Blocks across each cascade's depth.
    Vec4 radius{1.0F};     // Half the width of each cascade, in blocks.
    Vec4 toLight{0.0F};    // xyz towards the light, w strength (0 is off).
    Vec4 settings{0.0F};   // x cascades, y filter radius in texels, z unused, w the distance.
};
// Uniform ranges must start on 256 bytes for every driver.
constexpr usize kShadowCasterStride = 256;
// Texture layers the animation table covers: the 14 bit layer field of a quad.
constexpr usize kMaxTextureLayers = 16384;
static_assert(sizeof(IndirectDraw) == 20);

// First capacities of the draw list regions; a full region doubles.
constexpr std::array<u32, kChunkPassCount> kFirstRegionCapacity = {8192, 2048, 2048, 2048};
// CPU write memory per frame for copying changed draws to the GPU; larger
// changes go through Device::updateBuffer.
constexpr usize kDrawStagingBytes = usize{1} << 20U;

// Per chunk shader data (ChunkInfo in render/chunk.vert and chunk_cull.comp).
struct ChunkInfo {
    glm::ivec4 position{0}; // xyz: chunk position, w: ChunkBounds::pack().
};
static_assert(sizeof(ChunkInfo) == 16);

struct ChunkParams {
    // x: AO strength, y: daylight, z: darkest brightness, w: face shading (1 on, 0 off).
    Vec4 params{1.0F, 1.0F, 0.0F, 1.0F};
    Vec4 skyLight{1.0F};                       // rgb: the color of sky light right now.
    std::array<Vec4, kBlockTintKinds> tints{}; // Linear colors per tint kind.
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
    glm::uvec4 passCount{0};
    glm::uvec4 flags{0};
    Vec4 tuning{0.0F};
};
static_assert(sizeof(CullParams) == (6 * 16) + (2 * 16) + 64 + (7 * 16));

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
    , m_brightness(desc.settings->registerFloat({{.id = "Graphics.Lighting.Brightness",
                                                  .displayName = "Brightness",
                                                  .description = "How bright the darkest places (caves, "
                                                                 "night) still are."},
                                                 0.08F,
                                                 0.0F,
                                                 0.5F,
                                                 0.01F}))
    , m_noShading(desc.settings->registerBool(
          {{.id = "Graphics.Lighting.NoShading",
            .displayName = "No shading",
            .description = "Removes block shading and makes the game more vibrant."},
           false}))
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
    m_slotDraws.resize(desc.maxChunks);
    for (auto& draws : m_slotDraws) {
        draws.fill(kNoDraw);
    }
    m_translucentIndex.assign(desc.maxChunks, kNoDraw);
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
    auto animations = device.createBuffer({.sizeBytes = kMaxTextureLayers * sizeof(u32),
                                           .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                           .debugName = "Block texture animations"});
    if (!animations) {
        return animations.error().withContext("Chunk renderer");
    }
    r.m_animations = animations.value();
    const std::vector<u32> still(kMaxTextureLayers, 0);
    VXE_TRY(device.updateBuffer(r.m_animations, 0, std::as_bytes(std::span(still))));
    auto looks = device.createBuffer({.sizeBytes = kMaxTextureLayers * sizeof(u32),
                                      .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                      .debugName = "Block texture looks"});
    if (!looks) {
        return looks.error().withContext("Chunk renderer");
    }
    r.m_looks = looks.value();
    VXE_TRY(device.updateBuffer(r.m_looks, 0, std::as_bytes(std::span(still))));
    r.m_tints.fill(Vec4(1.0F));
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
        std::vector<rhi::ShaderDefine>{{.name = "VXE_CHUNK_CUTOUT"}},
        std::vector<rhi::ShaderDefine>{{.name = "VXE_CHUNK_TRANSLUCENT"}},
    };
    constexpr std::array<std::string_view, kChunkPassCount> kNames = {"Chunks opaque", "Chunks cutout",
                                                                      "Chunks plants", "Chunks translucent"};
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
        if (static_cast<ChunkPass>(pass) == ChunkPass::Plants) {
            pipeline.raster.cullMode = rhi::CullMode::None; // A plant has no back.
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
    // Shadow casters: depth only, both sides (the faces are chosen per light).
    for (usize pass = 0; pass < kSortedPassCount && built; ++pass) {
        std::vector<rhi::ShaderDefine> shadowDefines = defines.at(pass);
        shadowDefines.push_back({.name = "VXE_CHUNK_SHADOW"});
        const auto vertexShadow =
            rhi::createShaderFromFile(device, "render/chunk.vert", desc.shaders, shadowDefines);
        const auto fragmentShadow =
            rhi::createShaderFromFile(device, "render/chunk.frag", desc.shaders, shadowDefines);
        if (!vertexShadow || !fragmentShadow) {
            built = !vertexShadow ? vertexShadow.error() : fragmentShadow.error();
            if (vertexShadow) {
                device.destroy(vertexShadow.value());
            }
            if (fragmentShadow) {
                device.destroy(fragmentShadow.value());
            }
            break;
        }
        rhi::PipelineDesc pipeline{
            .vertex = vertexShadow.value(), .fragment = fragmentShadow.value(), .debugName = "Chunk shadows"};
        pipeline.colorCount = 0;
        pipeline.depthFormat = rhi::Format::D32Float;
        pipeline.raster.cullMode = rhi::CullMode::None;
        auto created = device.createPipeline(pipeline);
        device.destroy(vertexShadow.value());
        device.destroy(fragmentShadow.value());
        if (created) {
            r.m_shadowPipelines.at(pass) = created.value();
        } else {
            built = created.error();
        }
    }
    device.destroy(vertex.value());
    if (!built) {
        return built.error().withContext("Chunk renderer");
    }
    r.m_shadowSettings = std::make_unique<ShadowSettings>(*desc.settings);
    auto shadowSampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                               .magFilter = rhi::Filter::Nearest,
                                               .mipmapMode = rhi::MipmapMode::None,
                                               .addressU = rhi::AddressMode::ClampToEdge,
                                               .addressV = rhi::AddressMode::ClampToEdge,
                                               .debugName = "Shadow map sampler"});
    if (!shadowSampler) {
        return shadowSampler.error().withContext("Chunk renderer");
    }
    r.m_shadowSampler = shadowSampler.value();
    VXE_TRY(r.m_shadowParams.create(device, sizeof(ShadowParams), BufferUsage::Uniform, "Shadow parameters"));
    VXE_TRY(r.m_shadowCasters.create(device, kShadowCasterStride * kShadowMaxCascades, BufferUsage::Uniform,
                                     "Shadow cascade matrices"));
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
    VXE_TRY(
        m_drawStaging.create(m_device, kDrawStagingBytes, BufferUsage::TransferSrc, "Chunk draw changes"));
    if (!relayoutDraws(kFirstRegionCapacity)) {
        return Error(ErrorCode::OutOfMemory, "Could not create the chunk draw buffers");
    }
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
    m_shadowParams.destroy(m_device);
    m_shadowCasters.destroy(m_device);
    m_shadowDraws.destroy(m_device);
    for (const rhi::PipelineHandle pipeline : m_shadowPipelines) {
        if (pipeline.isValid()) {
            m_device.destroy(pipeline);
        }
    }
    if (m_shadowSampler.isValid()) {
        m_device.destroy(m_shadowSampler);
    }
    m_staging.destroy(m_device);
    m_cullParams.destroy(m_device);
    m_cpuDraws.destroy(m_device);
    m_drawStaging.destroy(m_device);
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
    for (const rhi::BufferHandle buffer :
         {m_arena, m_metadata, m_indices, m_draws, m_drawFaces, m_survivors, m_params, m_animations, m_looks,
          m_caveBitsBuffer, m_cullCounts, m_zeros}) {
        if (buffer.isValid()) {
            m_device.destroy(buffer);
        }
    }
}

void ChunkRenderer::setTextures(rhi::TextureHandle textures, rhi::SamplerHandle sampler) noexcept {
    m_textures = textures;
    m_sampler = sampler;
}

void ChunkRenderer::setTextureAnimations(std::span<const u32> animations) {
    std::vector<u32> table(kMaxTextureLayers, 0);
    std::ranges::copy(animations.first(std::min(animations.size(), kMaxTextureLayers)), table.begin());
    static_cast<void>(m_device.updateBuffer(m_animations, 0, std::as_bytes(std::span(table))));
}

void ChunkRenderer::setTextureLooks(std::span<const u32> looks) {
    std::vector<u32> table(kMaxTextureLayers, 0);
    std::ranges::copy(looks.first(std::min(looks.size(), kMaxTextureLayers)), table.begin());
    static_cast<void>(m_device.updateBuffer(m_looks, 0, std::as_bytes(std::span(table))));
}

void ChunkRenderer::setTintColors(std::span<const u32> colors) {
    // The exact sRGB curve, as the GPU decodes block textures.
    const auto linear = [](u32 value) {
        const f32 c = static_cast<f32>(value & 255U) / 255.0F;
        return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
    };
    m_tints.fill(Vec4(1.0F));
    for (usize kind = 1; kind < std::min(colors.size(), m_tints.size()); ++kind) {
        const u32 color = colors[kind];
        m_tints.at(kind) = Vec4(linear(color), linear(color >> 8U), linear(color >> 16U), 1.0F);
    }
    m_lastParams = Vec4(-1.0F); // Written with the next frame's parameters.
}

void ChunkRenderer::setDaylight(f32 daylight) noexcept {
    m_daylight = std::clamp(daylight, 0.0F, 1.0F);
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
    m_drawStagingMemory = m_drawStaging.next(m_device);
    m_drawStagingUsed = 0;
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
        m_translucentDirty = true; // The far to near order depends on it.
    }
    if (m_freezeCulling.get()) {
        if (!m_frozenCamera) {
            m_frozenCamera = camera;
            m_frozenAspect = viewAspect();
        }
    } else {
        m_frozenCamera.reset();
    }
    const ChunkParams params{.params = Vec4(m_aoStrength.get(), m_daylight, brightness(), faceShading()),
                             .skyLight = Vec4(m_skyLight, m_wetness),
                             .tints = m_tints};
    if (params.params != m_lastParams || params.skyLight != m_lastSkyLight) {
        m_lastParams = params.params;
        m_lastSkyLight = params.skyLight;
        static_cast<void>(m_device.updateBuffer(m_params, 0, std::as_bytes(std::span(&params, 1))));
    }
    // Shadows: the cascades for this camera and light, and what the chunk passes read.
    const bool shadowsOn = m_shadowSettings && m_shadowSettings->enabled() && m_shadowLight.strength > 0.0F;
    m_shadowFrame =
        shadowsOn ? computeShadowFrame(camera, viewAspect(), m_shadowLight.toLight, m_shadowLight.strength,
                                       m_shadowSettings->cascades(), m_shadowSettings->distance())
                  : ShadowFrame{};
    if (m_shadowParams.bytesPerFrame() != 0) {
        ShadowParams shadow;
        for (u32 i = 0; i < m_shadowFrame.cascades; ++i) {
            const ShadowCascade& cascade = m_shadowFrame.cascade.at(i);
            shadow.matrices.at(i) = cascade.matrix;
            shadow.splits[static_cast<glm::length_t>(i)] = cascade.far;
            shadow.depthRange[static_cast<glm::length_t>(i)] = cascade.depthRange;
            shadow.radius[static_cast<glm::length_t>(i)] = cascade.radius;
        }
        shadow.toLight = Vec4(m_shadowFrame.toLight, m_shadowFrame.enabled ? m_shadowFrame.strength : 0.0F);
        const f32 filter = m_shadowSettings ? static_cast<f32>(m_shadowSettings->quality() - 1) : 0.0F;
        shadow.settings = Vec4(static_cast<f32>(m_shadowFrame.cascades), filter, 0.0F,
                               m_shadowSettings ? m_shadowSettings->distance() : 0.0F);
        const std::span<std::byte> memory = m_shadowParams.next(m_device);
        std::memcpy(memory.data(), &shadow, sizeof(shadow));
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
    VXE_ASSERT(geometry.quads.size() % kChunkQuadBytes == 0, "Quads are 12 bytes each");
    const usize bytes = geometry.quads.size();
    if (bytes == 0) {
        // Nothing to draw, but the chunk may still block the view.
        if (const auto it = m_chunks.find(geometry.position); it != m_chunks.end()) {
            removeDraws(it->second.slot);
            release(it->second);
            m_chunks.erase(it);
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
        removeDraws(chunk.slot);
        m_residentQuads -= m_allocator.sizeOf(chunk.range);
        m_deferred.push_back({.frame = m_frame, .range = chunk.range, .slot = OffsetAllocator::kNoSpace});
        chunk.range = range;
        chunk.groupBegin = geometry.groupBegin;
        writeMetadata(chunk.slot, geometry.position, geometry.bounds);
        addDraws(chunk);
    } else {
        const u32 slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        writeMetadata(slot, geometry.position, geometry.bounds);
        const auto inserted = m_chunks
                                  .emplace(geometry.position, Resident{.position = geometry.position,
                                                                       .range = range,
                                                                       .slot = slot,
                                                                       .groupBegin = geometry.groupBegin})
                                  .first;
        addDraws(inserted->second);
        m_caveBitsDirty = true;
    }
    m_caves.setChunk(geometry.position, geometry.connectivity);
    m_residentQuads += quads;
    return UploadResult::Uploaded;
}

bool ChunkRenderer::remove(ChunkPos position) {
    m_caves.removeChunk(position);
    const auto it = m_chunks.find(position);
    if (it == m_chunks.end()) {
        return false;
    }
    removeDraws(it->second.slot);
    release(it->second);
    m_chunks.erase(it);
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
    flushDraws(commands);
    publishStats();
}

// The draw list. Every non empty face group of a resident chunk is one
// candidate draw. The list has a region per pass (opaque, cutout,
// translucent), each a dense array with room to grow, so the culling shader
// and the draws address a pass by its region start. Opaque and cutout
// entries are in no particular order: a chunk adds its entries at the end of
// a region, and removing one moves the region's last entry into the gap, so
// a chunk change touches a dozen entries instead of rebuilding the list (with
// chunks streaming in every frame, a full rebuild cost 0.55 ms on average
// and up to 3.5 ms). m_slotDraws remembers where each chunk's entries are.
// Translucent entries must stay far to near, so that region is sorted again
// when the camera changes chunk or a chunk with translucent quads changes;
// only those chunks are walked. Changed entries reach the GPU as copies from
// a small staging ring in flushDraws().

IndirectDraw ChunkRenderer::makeDraw(const Resident& chunk, usize group) noexcept {
    const u32 first = chunk.groupBegin.at(group);
    const u32 count = std::min(chunk.groupBegin.at(group + 1) - first, kMaxQuadsPerDraw);
    return {.indexCount = count * kIndicesPerQuad,
            .instanceCount = 1,
            .firstIndex = 0,
            .baseVertex = static_cast<i32>((chunk.range.offset + first) * 4),
            .baseInstance = chunk.slot};
}

void ChunkRenderer::markDrawsChanged(u32 first, u32 count) {
    if (count > 0) {
        m_dirtyDraws.emplace_back(first, first + count);
    }
}

void ChunkRenderer::addDraws(const Resident& chunk) {
    for (usize pass = 0; pass < kSortedPassCount; ++pass) {
        for (u32 face = 0; face < kChunkFaceCount; ++face) {
            const usize group = (pass * kChunkFaceCount) + face;
            if (chunk.groupBegin.at(group + 1) == chunk.groupBegin.at(group)) {
                continue;
            }
            if (m_drawCounts.at(pass) == m_regionCapacity.at(pass)) {
                std::array<u32, kChunkPassCount> grown = m_regionCapacity;
                grown.at(pass) *= 2;
                if (!relayoutDraws(grown)) {
                    return; // Out of GPU memory: the chunk stays undrawn.
                }
            }
            const u32 index = m_drawFirst.at(pass) + m_drawCounts.at(pass)++;
            m_drawData[index] = makeDraw(chunk, group);
            m_drawFaceData[index] = face;
            m_slotDraws[chunk.slot].at(group) = index;
            markDrawsChanged(index, 1);
        }
    }
    const usize translucent = static_cast<usize>(ChunkPass::Translucent) * kChunkFaceCount;
    if (chunk.groupBegin.at(translucent + kChunkFaceCount) > chunk.groupBegin.at(translucent)) {
        m_translucentIndex[chunk.slot] = static_cast<u32>(m_translucentSlots.size());
        m_translucentSlots.push_back(chunk.slot);
        m_translucentDirty = true;
    }
}

void ChunkRenderer::removeDraws(u32 slot) {
    std::array<u32, kSortedPassCount * kChunkFaceCount>& draws = m_slotDraws[slot];
    for (usize group = 0; group < draws.size(); ++group) {
        const u32 index = draws.at(group);
        if (index == kNoDraw) {
            continue;
        }
        draws.at(group) = kNoDraw;
        const usize pass = group / kChunkFaceCount;
        const u32 last = m_drawFirst.at(pass) + --m_drawCounts.at(pass);
        if (index != last) {
            // The region's last entry fills the gap; its chunk learns where.
            m_drawData[index] = m_drawData[last];
            m_drawFaceData[index] = m_drawFaceData[last];
            const usize movedGroup = (pass * kChunkFaceCount) + m_drawFaceData[index];
            m_slotDraws[m_drawData[index].baseInstance].at(movedGroup) = index;
            markDrawsChanged(index, 1);
        }
    }
    if (const u32 place = m_translucentIndex[slot]; place != kNoDraw) {
        const u32 moved = m_translucentSlots.back();
        m_translucentSlots[place] = moved;
        m_translucentIndex[moved] = place;
        m_translucentSlots.pop_back();
        m_translucentIndex[slot] = kNoDraw;
        m_translucentDirty = true;
    }
}

// Gives the regions new capacities: new GPU buffers (the device destroys the
// old ones once the GPU is done with them) and every entry moved to its
// region's new start. The whole list goes to the GPU at the next flush.
bool ChunkRenderer::relayoutDraws(const std::array<u32, kChunkPassCount>& capacities) {
    VXE_PROFILE_FUNCTION();
    usize total = 0;
    std::array<u32, kChunkPassCount> first{};
    for (usize pass = 0; pass < kChunkPassCount; ++pass) {
        first.at(pass) = static_cast<u32>(total);
        total += capacities.at(pass);
    }
    rhi::BufferHandle draws;
    rhi::BufferHandle faces;
    rhi::BufferHandle survivors;
    auto createdDraws = m_device.createBuffer({.sizeBytes = total * sizeof(IndirectDraw),
                                               .usage = BufferUsage::Indirect | BufferUsage::Storage |
                                                        BufferUsage::TransferDst | BufferUsage::TransferSrc,
                                               .debugName = "Chunk draws"});
    auto createdFaces = m_device.createBuffer({.sizeBytes = total * sizeof(u32),
                                               .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                               .debugName = "Chunk draw faces"});
    auto createdSurvivors = m_device.createBuffer(
        {.sizeBytes = total * sizeof(IndirectDraw),
         .usage = BufferUsage::Indirect | BufferUsage::Storage | BufferUsage::TransferSrc,
         .debugName = "Chunk draws after culling"});
    if (createdDraws) {
        draws = createdDraws.value();
    }
    if (createdFaces) {
        faces = createdFaces.value();
    }
    if (createdSurvivors) {
        survivors = createdSurvivors.value();
    }
    if (!createdDraws || !createdFaces || !createdSurvivors) {
        for (const rhi::BufferHandle buffer : {draws, faces, survivors}) {
            if (buffer.isValid()) {
                m_device.destroy(buffer);
            }
        }
        return false;
    }
    for (const rhi::BufferHandle buffer : {m_draws, m_drawFaces, m_survivors}) {
        if (buffer.isValid()) {
            m_device.destroy(buffer);
        }
    }
    m_draws = draws;
    m_drawFaces = faces;
    m_survivors = survivors;

    std::vector<IndirectDraw> drawData(total);
    std::vector<u32> faceData(total, 0);
    for (usize pass = 0; pass < kChunkPassCount; ++pass) {
        const u32 from = m_drawFirst.at(pass);
        const u32 to = first.at(pass);
        const u32 count = m_drawCounts.at(pass);
        std::copy_n(m_drawData.begin() + from, count, drawData.begin() + to);
        std::copy_n(m_drawFaceData.begin() + from, count, faceData.begin() + to);
        if (pass < kSortedPassCount) {
            for (u32 i = 0; i < count; ++i) {
                const usize group = (pass * kChunkFaceCount) + faceData[to + i];
                m_slotDraws[drawData[to + i].baseInstance].at(group) = to + i;
            }
        }
    }
    m_drawData = std::move(drawData);
    m_drawFaceData = std::move(faceData);
    m_drawFirst = first;
    m_regionCapacity = capacities;
    m_drawCapacity = total;
    m_dirtyDraws.clear();
    m_layoutDirty = true;
    return true;
}

// Writes the translucent region far to near by chunk, so nearer water and
// glass blend over farther ones.
void ChunkRenderer::sortTranslucent() {
    VXE_PROFILE_FUNCTION();
    m_translucentDirty = false;
    m_sortScratch.clear();
    u32 needed = 0;
    for (const u32 slot : m_translucentSlots) {
        const ChunkPos position = m_slotInfo[slot].position;
        const i64 dx = static_cast<i64>(position.x) - m_cameraChunk.x;
        const i64 dy = static_cast<i64>(position.y) - m_cameraChunk.y;
        const i64 dz = static_cast<i64>(position.z) - m_cameraChunk.z;
        m_sortScratch.emplace_back((dx * dx) + (dy * dy) + (dz * dz), slot);
        needed += kChunkFaceCount;
    }
    constexpr auto kTranslucent = static_cast<usize>(ChunkPass::Translucent);
    if (needed > m_regionCapacity.at(kTranslucent)) {
        std::array<u32, kChunkPassCount> grown = m_regionCapacity;
        while (grown.at(kTranslucent) < needed) {
            grown.at(kTranslucent) *= 2;
        }
        if (!relayoutDraws(grown)) {
            m_drawCounts.at(kTranslucent) = 0;
            return;
        }
    }
    std::ranges::sort(m_sortScratch, [](const auto& a, const auto& b) { return a.first > b.first; });
    const u32 first = m_drawFirst.at(kTranslucent);
    u32 count = 0;
    for (const auto& [distance, slot] : m_sortScratch) {
        const auto it = m_chunks.find(m_slotInfo[slot].position);
        VXE_ASSERT(it != m_chunks.end(), "A translucent slot without its chunk");
        for (u32 face = 0; face < kChunkFaceCount; ++face) {
            const usize group = (kTranslucent * kChunkFaceCount) + face;
            if (it->second.groupBegin.at(group + 1) == it->second.groupBegin.at(group)) {
                continue;
            }
            m_drawData[first + count] = makeDraw(it->second, group);
            m_drawFaceData[first + count] = face;
            ++count;
        }
    }
    m_drawCounts.at(kTranslucent) = count;
    markDrawsChanged(first, count);
}

// Sends this frame's draw list changes to the GPU: copies of the changed
// ranges through the draw staging ring, or the whole list after a relayout.
// Record outside render passes.
void ChunkRenderer::flushDraws(rhi::CommandList& commands) {
    if (m_translucentDirty) {
        sortTranslucent();
    }
    if (m_layoutDirty) {
        m_layoutDirty = false;
        m_dirtyDraws.clear();
        static_cast<void>(m_device.updateBuffer(m_draws, 0, std::as_bytes(std::span(m_drawData))));
        static_cast<void>(m_device.updateBuffer(m_drawFaces, 0, std::as_bytes(std::span(m_drawFaceData))));
    } else if (!m_dirtyDraws.empty()) {
        // Overlapping and touching ranges become one copy.
        std::ranges::sort(m_dirtyDraws);
        usize merged = 0;
        for (usize i = 1; i < m_dirtyDraws.size(); ++i) {
            if (m_dirtyDraws[i].first <= m_dirtyDraws[merged].second) {
                m_dirtyDraws[merged].second = std::max(m_dirtyDraws[merged].second, m_dirtyDraws[i].second);
            } else {
                m_dirtyDraws[++merged] = m_dirtyDraws[i];
            }
        }
        m_dirtyDraws.resize(merged + 1);
        bool copied = false;
        for (const auto& [begin, end] : m_dirtyDraws) {
            const auto drawBytes = std::as_bytes(std::span(m_drawData).subspan(begin, end - begin));
            const auto faceBytes = std::as_bytes(std::span(m_drawFaceData).subspan(begin, end - begin));
            if (m_drawStagingUsed + drawBytes.size() + faceBytes.size() <= m_drawStagingMemory.size()) {
                std::memcpy(m_drawStagingMemory.data() + m_drawStagingUsed, drawBytes.data(),
                            drawBytes.size());
                commands.copyBuffer(m_drawStaging.current(), m_drawStagingUsed, m_draws,
                                    usize{begin} * sizeof(IndirectDraw), drawBytes.size());
                m_drawStagingUsed += drawBytes.size();
                std::memcpy(m_drawStagingMemory.data() + m_drawStagingUsed, faceBytes.data(),
                            faceBytes.size());
                commands.copyBuffer(m_drawStaging.current(), m_drawStagingUsed, m_drawFaces,
                                    usize{begin} * sizeof(u32), faceBytes.size());
                m_drawStagingUsed += faceBytes.size();
                copied = true;
            } else {
                static_cast<void>(
                    m_device.updateBuffer(m_draws, usize{begin} * sizeof(IndirectDraw), drawBytes));
                static_cast<void>(m_device.updateBuffer(m_drawFaces, usize{begin} * sizeof(u32), faceBytes));
            }
        }
        if (copied) {
            commands.resourceBarrier(rhi::BarrierScope::StorageData | rhi::BarrierScope::IndirectCommands);
        }
        m_dirtyDraws.clear();
    }
    m_gpuDrawCounts = m_drawCounts;
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
    flushDraws(commands);
    m_culledFrame = m_frame;
    const Camera& cullCamera = m_frozenCamera ? *m_frozenCamera : m_camera;
    if (m_caveCulling.get()) {
        updateCaveBits(cullCamera);
    }
    if (m_gpuDrawCounts[0] + m_gpuDrawCounts[1] + m_gpuDrawCounts[2] == 0) {
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
    // Threads cover the regions up to the end of the translucent entries;
    // the shader skips the unused room at the end of each region.
    const u32 total = m_drawFirst[3] + m_gpuDrawCounts[3];
    const u32 candidates = m_gpuDrawCounts[0] + m_gpuDrawCounts[1] + m_gpuDrawCounts[2] + m_gpuDrawCounts[3];
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
                (caveCullingOn() ? kTestCave : 0U);
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
    params.passFirst = glm::uvec4(m_drawFirst[0], m_drawFirst[1], m_drawFirst[2], m_drawFirst[3]);
    params.passCount =
        glm::uvec4(m_gpuDrawCounts[0], m_gpuDrawCounts[1], m_gpuDrawCounts[2], m_gpuDrawCounts[3]);
    params.flags = glm::uvec4(tests, total, 0, 0);
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
            readback.candidates = candidates;
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
    const bool caveOn = caveCullingOn();
    const f32 padding = m_frustumPadding.get();
    ChunkCullingCounts counts{.onGpu = false};
    // A chunk's face groups are mostly next to each other in the list (a
    // chunk adds them together; only removals move single entries), so the
    // box and the frustum test are worked out about once per chunk.
    u32 boxSlot = OffsetAllocator::kNoSpace;
    Vec3 boxMin(0.0F);
    Vec3 boxMax(0.0F);
    bool inFrustum = true;
    for (usize pass = 0; pass < kChunkPassCount; ++pass) {
        const u32 first = m_drawFirst.at(pass);
        u32 kept = 0;
        for (u32 i = first; i < first + m_drawCounts.at(pass); ++i) {
            const IndirectDraw& draw = m_drawData[i];
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
            if (faceOn && pass < static_cast<usize>(ChunkPass::Plants) &&
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
    // Changes after the last flush wait for the next frame: the GPU copy of
    // the list is what the GPU draws from, and stale entries stay valid
    // because freed ranges and slots are only reused frames later.
    const auto index = static_cast<usize>(pass);
    const u32 count = m_gpuDrawCounts.at(index);
    if (count == 0 || !m_textures.isValid() ||
        (m_cullPath == CullPath::Cpu && m_cpuDrawCounts.at(index) == 0)) {
        return;
    }
    commands.bindPipeline(m_wireframe.get() ? m_wireframePipelines.at(index) : m_pipelines.at(index));
    commands.bindUniformBuffer(1, 0, m_params, 0, sizeof(ChunkParams));
    commands.bindStorageBuffer(2, 0, m_arena, 0, m_arenaBytes);
    commands.bindStorageBuffer(2, 1, m_metadata, 0, usize{m_maxChunks} * sizeof(ChunkInfo));
    commands.bindTexture(2, 2, m_textures, m_sampler);
    commands.bindStorageBuffer(2, 3, m_animations, 0, kMaxTextureLayers * sizeof(u32));
    commands.bindStorageBuffer(2, 4, m_looks, 0, kMaxTextureLayers * sizeof(u32));
    // The shadow map, or the block textures as a stand in the shader never reads.
    bindShadows(commands);
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

void ChunkRenderer::recordShadows(rhi::CommandList& commands) {
    VXE_PROFILE_FUNCTION();
    m_shadowCounts = {};
    m_shadowDrawTotal = 0;
    if (!m_shadowFrame.enabled || !m_textures.isValid() || m_drawCapacity == 0) {
        return;
    }
    const usize perCascade = m_drawCapacity * sizeof(IndirectDraw);
    if (m_shadowDraws.bytesPerFrame() < perCascade * kShadowMaxCascades) {
        m_shadowDraws.destroy(m_device);
        if (!m_shadowDraws.create(m_device, perCascade * kShadowMaxCascades, BufferUsage::Indirect,
                                  "Shadow draws")) {
            return;
        }
    }
    const std::span<std::byte> draws = m_shadowDraws.next(m_device);
    const std::span<std::byte> casters = m_shadowCasters.next(m_device);
    const CameraPosition camera = m_camera.renderPosition();
    const Vec3 toLight = m_shadowFrame.toLight;
    const bool caveOn = caveCullingOn() && !m_caveBits.empty();
    // A chunk only matters if its shadow can fall into view: its box, swept
    // away from the light, must touch the camera's frustum. Most of a cascade
    // lies beside or behind the view, so this skips most of it.
    const Frustum view = Frustum::fromViewProjection(m_camera.viewProjection(viewAspect()));
    const Vec3 sweep = -toLight * kShadowCasterReach;
    // A face casts from its back: only faces turned away from the light go in,
    // so a lit surface never meets its own depth and needs almost no bias.
    const std::array<Vec3, kChunkFaceCount> kNormals = {Vec3(1, 0, 0),  Vec3(-1, 0, 0), Vec3(0, 1, 0),
                                                        Vec3(0, -1, 0), Vec3(0, 0, 1),  Vec3(0, 0, -1)};
    for (u32 c = 0; c < m_shadowFrame.cascades; ++c) {
        const Mat4& m = m_shadowFrame.cascade.at(c).matrix;
        std::memcpy(casters.data() + (usize{c} * kShadowCasterStride), &m, sizeof(m));
        std::byte* out = draws.data() + (usize{c} * perCascade);
        u32 written = 0;
        u32 boxSlot = OffsetAllocator::kNoSpace;
        bool inside = false;
        for (usize pass = 0; pass < kSortedPassCount; ++pass) {
            const u32 first = m_drawFirst.at(pass);
            u32 kept = 0;
            // Grass and flowers are most of the quads but small shadows: only
            // the nearest cascade draws them.
            const u32 end = pass == static_cast<usize>(ChunkPass::Plants) && c > 0
                                ? first
                                : first + m_drawCounts.at(pass);
            for (u32 i = first; i < end; ++i) {
                const IndirectDraw& draw = m_drawData[i];
                if (draw.baseInstance != boxSlot) {
                    boxSlot = draw.baseInstance;
                    const SlotInfo& info = m_slotInfo[boxSlot];
                    const Vec3 origin = relativeToCamera(info.position, camera);
                    const Vec3 low =
                        origin + Vec3(info.bounds.min[0], info.bounds.min[1], info.bounds.min[2]);
                    const Vec3 high =
                        origin + Vec3(info.bounds.max[0], info.bounds.max[1], info.bounds.max[2]);
                    const Vec3 center = (low + high) * 0.5F;
                    const Vec3 half = (high - low) * 0.5F;
                    const Vec4 p = m * Vec4(center, 1.0F);
                    const auto reach = [&](int row) {
                        return (std::abs(m[0][row]) * half.x) + (std::abs(m[1][row]) * half.y) +
                               (std::abs(m[2][row]) * half.z);
                    };
                    inside = std::abs(p.x) <= 1.0F + reach(0) && std::abs(p.y) <= 1.0F + reach(1) &&
                             p.z + reach(2) >= 0.0F && p.z - reach(2) <= 1.0F &&
                             view.intersects(Aabb{.min = glm::min(low, low + sweep),
                                                  .max = glm::max(high, high + sweep)});
                }
                if (!inside) {
                    continue;
                }
                // Closed caves sit under rock that already stops the light: only
                // chunks the cave search reached through open air cast.
                if (caveOn &&
                    (m_caveBits[draw.baseInstance >> 5U] & (1U << (draw.baseInstance & 31U))) == 0) {
                    continue;
                }
                if (pass < static_cast<usize>(ChunkPass::Plants) &&
                    glm::dot(kNormals.at(m_drawFaceData[i]), toLight) >= 0.0F) {
                    continue;
                }
                std::memcpy(out + (usize{first + kept} * sizeof(IndirectDraw)), &draw, sizeof(draw));
                ++kept;
            }
            m_shadowCounts.at(c).at(pass) = kept;
            written += kept;
        }
        m_shadowDrawTotal += written;
    }
    const auto tile = static_cast<f32>(kShadowTileSize);
    for (u32 c = 0; c < m_shadowFrame.cascades; ++c) {
        commands.setViewport(static_cast<f32>(c % 2U) * tile, static_cast<f32>(c / 2U) * tile, tile, tile);
        for (usize pass = 0; pass < kSortedPassCount; ++pass) {
            const u32 count = m_shadowCounts.at(c).at(pass);
            if (count == 0) {
                continue;
            }
            // Past the nearest cascade leaves cast as whole blocks: their holes
            // are smaller than a texel there, and the alpha test is the cost.
            const bool solid = c > 0 && pass == static_cast<usize>(ChunkPass::Cutout);
            commands.bindPipeline(m_shadowPipelines.at(solid ? static_cast<usize>(ChunkPass::Opaque) : pass));
            commands.bindUniformBuffer(1, 0, m_params, 0, sizeof(ChunkParams));
            commands.bindUniformBuffer(1, 3, m_shadowCasters.current(), usize{c} * kShadowCasterStride,
                                       sizeof(Mat4));
            commands.bindStorageBuffer(2, 0, m_arena, 0, m_arenaBytes);
            commands.bindStorageBuffer(2, 1, m_metadata, 0, usize{m_maxChunks} * sizeof(ChunkInfo));
            commands.bindTexture(2, 2, m_textures, m_sampler);
            commands.bindStorageBuffer(2, 3, m_animations, 0, kMaxTextureLayers * sizeof(u32));
            commands.bindStorageBuffer(2, 4, m_looks, 0, kMaxTextureLayers * sizeof(u32));
            commands.bindIndexBuffer(m_indices, rhi::IndexType::Uint16);
            const usize offset =
                (usize{c} * perCascade) + (usize{m_drawFirst.at(pass)} * sizeof(IndirectDraw));
            commands.multiDrawIndexedIndirect(m_shadowDraws.current(), offset, count);
        }
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
    std::vector<u32> averages;
    averages.reserve(layers);
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
                averages.push_back(level.front());
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
    return BlockTextures{
        .texture = texture.value(), .sampler = sampler.value(), .averageColors = std::move(averages)};
}

void ChunkRenderer::bindShadows(rhi::CommandList& commands) const {
    commands.bindUniformBuffer(1, 2, m_shadowParams.current(), 0, sizeof(ShadowParams));
    commands.bindTexture(2, 5, m_shadowMap.isValid() ? m_shadowMap : m_textures,
                         m_shadowMap.isValid() ? m_shadowSampler : m_sampler);
}

} // namespace vxe::render
