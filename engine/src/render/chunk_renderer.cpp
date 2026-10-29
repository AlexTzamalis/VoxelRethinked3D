#include "vxe/render/chunk_renderer.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <cstring>
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

// Per chunk shader data (ChunkInfo in render/chunk.vert).
struct ChunkInfo {
    glm::ivec4 position{0};
};
static_assert(sizeof(ChunkInfo) == 16);

struct ChunkParams {
    Vec4 params{1.0F, 0.0F, 0.0F, 0.0F}; // x: AO strength.
};

// Ranges freed in frame F may still be read by the GPU frames before it;
// they are reused once the staging ring proves those are done.
constexpr u64 kFramesInFlight = CpuWriteRing::kFrames;

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
                                               false})) {
    m_chunks.reserve(desc.maxChunks);
    m_freeSlots.reserve(desc.maxChunks);
    for (u32 slot = desc.maxChunks; slot > 0; --slot) {
        m_freeSlots.push_back(slot - 1);
    }
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
        return arena.error().withContext("Chunk renderer: ");
    }
    r.m_arena = arena.value();
    auto metadata = device.createBuffer(
        {.sizeBytes = usize{desc.maxChunks} * sizeof(ChunkInfo),
         .usage = BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc,
         .debugName = "Chunk metadata"});
    if (!metadata) {
        return metadata.error().withContext("Chunk renderer: ");
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
        return indexBuffer.error().withContext("Chunk renderer: ");
    }
    r.m_indices = indexBuffer.value();
    VXE_TRY(device.updateBuffer(r.m_indices, 0, std::as_bytes(std::span(indices))));

    auto params = device.createBuffer({.sizeBytes = sizeof(ChunkParams),
                                       .usage = BufferUsage::Uniform | BufferUsage::TransferDst,
                                       .debugName = "Chunk parameters"});
    if (!params) {
        return params.error().withContext("Chunk renderer: ");
    }
    r.m_params = params.value();
    VXE_TRY(r.m_staging.create(device, desc.stagingBytesPerFrame, BufferUsage::TransferSrc,
                               "Chunk upload staging"));

    // One pipeline per pass, plus wireframe versions for Debug.Terrain.Wireframe.
    const auto vertex = rhi::createShaderFromFile(device, "render/chunk.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Chunk renderer: ");
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
        return built.error().withContext("Chunk renderer: ");
    }

    if (r.m_stats != nullptr) {
        const auto add = [&r](const stats::StatDesc& statDesc) {
            auto id = r.m_stats->registerStat(statDesc);
            return id ? id.value() : stats::StatId{};
        };
        using stats::StatType;
        using stats::StatUnit;
        r.m_statIds = {
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
            .draws = add({.name = "Render.Chunks.Draws",
                          .description = "Indirect draws (face groups) per frame over all passes.",
                          .type = StatType::Gauge}),
            .uploadBytes = add({.name = "Render.Chunks.UploadBytes",
                                .description = "Chunk mesh bytes uploaded this frame.",
                                .unit = StatUnit::Bytes}),
            .overBudget = add({.name = "Render.Chunks.UploadsOverBudget",
                               .description = "Uploads refused this frame because the budget was used."}),
            .outOfMemory =
                add({.name = "Render.Chunks.UploadsOutOfMemory",
                     .description = "Uploads refused because the arena or the chunk slots were full."}),
        };
    }
    return renderer;
}

ChunkRenderer::~ChunkRenderer() {
    m_staging.destroy(m_device);
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
    for (const rhi::BufferHandle buffer : {m_arena, m_metadata, m_indices, m_draws, m_params}) {
        if (buffer.isValid()) {
            m_device.destroy(buffer);
        }
    }
}

void ChunkRenderer::setTextures(rhi::TextureHandle textures, rhi::SamplerHandle sampler) noexcept {
    m_textures = textures;
    m_sampler = sampler;
}

void ChunkRenderer::beginFrame(const Camera& camera) {
    VXE_PROFILE_FUNCTION();
    ++m_frame;
    m_stagingMemory = m_staging.next(m_device);
    m_stagingUsed = 0;
    m_pendingCopies.clear();
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
    const ChunkPos cameraChunk = camera.renderPosition().chunk;
    if (cameraChunk != m_cameraChunk) {
        m_cameraChunk = cameraChunk;
        m_drawsDirty = true; // The translucent order depends on it.
    }
    const ChunkParams params{.params = Vec4(m_aoStrength.get(), 0.0F, 0.0F, 0.0F)};
    if (params.params != m_lastParams) {
        m_lastParams = params.params;
        static_cast<void>(m_device.updateBuffer(m_params, 0, std::as_bytes(std::span(&params, 1))));
    }
}

ChunkRenderer::UploadResult ChunkRenderer::upload(const ChunkGeometry& geometry) {
    VXE_ASSERT(!m_stagingMemory.empty(), "Call beginFrame before uploading");
    VXE_ASSERT(geometry.quads.size() % kChunkQuadBytes == 0, "Quads are 8 bytes each");
    const usize bytes = geometry.quads.size();
    if (bytes == 0) {
        remove(geometry.position);
        return UploadResult::Uploaded;
    }
    // The budget may be passed by one chunk when nothing was uploaded yet
    // this frame; otherwise a chunk bigger than the budget would never fit.
    const usize budget =
        std::min(static_cast<usize>(std::max(m_uploadBudgetKb.get(), 1)) * 1024, m_stagingMemory.size());
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
    } else {
        const u32 slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        const ChunkInfo info{
            .position = glm::ivec4(geometry.position.x, geometry.position.y, geometry.position.z, 0)};
        static_cast<void>(m_device.updateBuffer(m_metadata, usize{slot} * sizeof(ChunkInfo),
                                                std::as_bytes(std::span(&info, 1))));
        m_chunks.emplace(geometry.position, Resident{.position = geometry.position,
                                                     .range = range,
                                                     .slot = slot,
                                                     .groupBegin = geometry.groupBegin});
    }
    m_residentQuads += quads;
    m_drawsDirty = true;
    return UploadResult::Uploaded;
}

bool ChunkRenderer::remove(ChunkPos position) {
    const auto it = m_chunks.find(position);
    if (it == m_chunks.end()) {
        return false;
    }
    release(it->second);
    m_chunks.erase(it);
    m_drawsDirty = true;
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
    m_drawData.clear();
    std::array<std::vector<IndirectDraw>, kChunkPassCount> perPass;
    const auto addDraws = [&](const Resident& chunk, usize pass) {
        for (usize face = 0; face < 6; ++face) {
            const usize group = (pass * 6) + face;
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
        }
    };
    m_sortScratch.clear();
    for (const auto& entry : m_chunks) {
        const Resident& chunk = entry.second;
        addDraws(chunk, static_cast<usize>(ChunkPass::Opaque));
        addDraws(chunk, static_cast<usize>(ChunkPass::Cutout));
        const usize translucent = static_cast<usize>(ChunkPass::Translucent) * 6;
        if (chunk.groupBegin.at(translucent + 6) > chunk.groupBegin.at(translucent)) {
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
    if (total > m_drawCapacity) {
        // Grow to the next power of two; the device destroys the old buffer
        // once the GPU is done with it.
        usize capacity = std::max<usize>(m_drawCapacity, 1024);
        while (capacity < total) {
            capacity *= 2;
        }
        if (m_draws.isValid()) {
            m_device.destroy(m_draws);
            m_draws = {};
        }
        auto buffer = m_device.createBuffer(
            {.sizeBytes = capacity * sizeof(IndirectDraw),
             .usage = BufferUsage::Indirect | BufferUsage::TransferDst | BufferUsage::TransferSrc,
             .debugName = "Chunk draws"});
        if (!buffer) {
            m_drawCapacity = 0;
            m_drawCounts.fill(0);
            return;
        }
        m_draws = buffer.value();
        m_drawCapacity = capacity;
    }
    for (const auto& draws : perPass) {
        const auto bytes = std::as_bytes(std::span(draws));
        m_drawData.insert(m_drawData.end(), bytes.begin(), bytes.end());
    }
    if (!m_drawData.empty()) {
        static_cast<void>(m_device.updateBuffer(m_draws, 0, m_drawData));
    }
}

void ChunkRenderer::record(rhi::CommandList& commands, ChunkPass pass) {
    if (m_drawsDirty) {
        rebuildDraws();
    }
    const auto index = static_cast<usize>(pass);
    const u32 count = m_drawCounts.at(index);
    if (count == 0 || !m_textures.isValid()) {
        return;
    }
    commands.bindPipeline(m_wireframe.get() ? m_wireframePipelines.at(index) : m_pipelines.at(index));
    commands.bindUniformBuffer(1, 0, m_params, 0, sizeof(ChunkParams));
    commands.bindStorageBuffer(2, 0, m_arena, 0, m_arenaBytes);
    commands.bindStorageBuffer(2, 1, m_metadata, 0, usize{m_maxChunks} * sizeof(ChunkInfo));
    commands.bindTexture(2, 2, m_textures, m_sampler);
    commands.bindIndexBuffer(m_indices, rhi::IndexType::Uint16);
    commands.multiDrawIndexedIndirect(m_draws, usize{m_drawFirst.at(index)} * sizeof(IndirectDraw), count);
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
