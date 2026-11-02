#include "vxe/render/lod_renderer.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/math/aabb.hpp>
#include <vxe/math/frustum.hpp>
#include <vxe/math/projection.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace vxe::render {
namespace {

using rhi::BufferUsage;

// Quads per indirect draw: the shared index buffer holds 16 bit indices, so
// one draw reaches 65,536 vertices. Bigger tiles take several draws.
constexpr u32 kMaxQuadsPerDraw = 16384;
constexpr u32 kIndicesPerQuad = 6;
// Arena ranges and tile slots are reused only after the GPU finished every
// frame that may still draw them.
constexpr u64 kFramesInFlight = CpuWriteRing::kFrames + 1;

struct TileInfo {
    glm::ivec4 origin; // See render/lod.vert.
};
static_assert(sizeof(TileInfo) == 16);

struct IndirectDraw {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    i32 baseVertex;
    u32 baseInstance;
};

struct LodParams {
    Vec4 params;
    Vec4 skyLight;
};

} // namespace

LodRenderer::LodRenderer(const LodRendererDesc& desc)
    : m_device(*desc.device)
    , m_stats(desc.stats)
    , m_arenaBytes(desc.arenaBytes)
    , m_maxTiles(desc.maxTiles)
    , m_allocator(static_cast<u32>(desc.arenaBytes / kLodQuadBytes), desc.maxTiles * 2)
    , m_uploadBudgetKb(
          desc.settings->registerInt({{.id = "Graphics.LOD.MaxUploadKBPerFrame",
                                       .displayName = "Far terrain upload budget",
                                       .description = "Far terrain mesh data sent to the GPU per "
                                                      "frame, so new tiles never cause a spike.",
                                       .visibility = config::Visibility::Developer},
                                      2048,
                                      64,
                                      static_cast<i32>(desc.stagingBytesPerFrame / 1024),
                                      64}))
    , m_coverageDiscard(desc.settings->registerBool(
          {{.id = "Graphics.LOD.CoverageDiscard",
            .displayName = "Hide far terrain under chunks",
            .description = "Far terrain is not drawn where real chunks are. Off shows both (debugging).",
            .visibility = config::Visibility::Developer},
           true}))
    , m_colorLevels(desc.settings->registerBool({{.id = "Debug.LOD.ColorLevels",
                                                  .displayName = "Color far terrain by level",
                                                  .description = "Each LOD level in its own color.",
                                                  .visibility = config::Visibility::Developer},
                                                 false}))
    , m_showCoverage(desc.settings->registerBool(
          {{.id = "Debug.LOD.ShowCoverage",
            .displayName = "Show far terrain coverage",
            .description = "Far terrain over columns real chunks draw shows magenta instead of hiding.",
            .visibility = config::Visibility::Developer},
           false}))
    , m_tileBorders(
          desc.settings->registerBool({{.id = "Debug.LOD.TileBorders",
                                        .displayName = "Far terrain tile borders",
                                        .description = "Outlines every far terrain tile, colored by level.",
                                        .visibility = config::Visibility::Developer},
                                       false})) {
    m_freeSlots.reserve(desc.maxTiles);
    for (u32 slot = desc.maxTiles; slot > 0; --slot) {
        m_freeSlots.push_back(slot - 1);
    }
    m_coverageBytes.assign(usize{kLodCoverageSize} * kLodCoverageSize, 0);
}

Result<std::unique_ptr<LodRenderer>> LodRenderer::create(const LodRendererDesc& desc) {
    VXE_ASSERT(desc.device != nullptr && desc.settings != nullptr, "LodRenderer needs a device and settings");
    std::unique_ptr<LodRenderer> renderer(new LodRenderer(desc));
    LodRenderer& r = *renderer;
    rhi::Device& device = *desc.device;

    auto arena = device.createBuffer({.sizeBytes = desc.arenaBytes,
                                      .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                      .debugName = "LOD vertex arena"});
    if (!arena) {
        return arena.error().withContext("LOD renderer");
    }
    r.m_arena = arena.value();

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
                                            .debugName = "LOD quad indices"});
    if (!indexBuffer) {
        return indexBuffer.error().withContext("LOD renderer");
    }
    r.m_indices = indexBuffer.value();
    VXE_TRY(device.updateBuffer(r.m_indices, 0, std::as_bytes(std::span(indices))));

    auto materials = device.createBuffer({.sizeBytes = kLodMaterialCount * sizeof(u32),
                                          .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                          .debugName = "LOD material colors"});
    if (!materials) {
        return materials.error().withContext("LOD renderer");
    }
    r.m_materials = materials.value();
    const std::vector<u32> magenta(kLodMaterialCount, 0xFFFF00FFU);
    VXE_TRY(device.updateBuffer(r.m_materials, 0, std::as_bytes(std::span(magenta))));

    auto params = device.createBuffer({.sizeBytes = sizeof(LodParams),
                                       .usage = BufferUsage::Uniform | BufferUsage::TransferDst,
                                       .debugName = "LOD parameters"});
    if (!params) {
        return params.error().withContext("LOD renderer");
    }
    r.m_params = params.value();

    auto coverage =
        device.createTexture({.format = rhi::Format::R8Unorm,
                              .width = kLodCoverageSize,
                              .height = kLodCoverageSize,
                              .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                              .debugName = "LOD coverage map"});
    if (!coverage) {
        return coverage.error().withContext("LOD renderer");
    }
    r.m_coverage = coverage.value();
    auto noTextures = device.createTexture({.type = rhi::TextureType::Texture2DArray,
                                            .format = rhi::Format::RGBA8Unorm,
                                            .width = 1,
                                            .height = 1,
                                            .usage = rhi::TextureUsage::Sampled,
                                            .debugName = "LOD no textures"});
    if (!noTextures) {
        return noTextures.error().withContext("LOD renderer");
    }
    r.m_noTextures = noTextures.value();
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .debugName = "LOD coverage"});
    if (!sampler) {
        return sampler.error().withContext("LOD renderer");
    }
    r.m_coverageSampler = sampler.value();

    VXE_TRY(r.m_staging.create(device, desc.stagingBytesPerFrame, BufferUsage::TransferSrc,
                               "LOD upload staging"));
    VXE_TRY(r.m_tileInfo.create(device, usize{desc.maxTiles} * sizeof(TileInfo), BufferUsage::Storage,
                                "LOD tiles (per frame)"));
    // A tile of up to 64 x 64 x 8 cells' worth of quads takes a few draws at most.
    VXE_TRY(r.m_draws.create(device, usize{desc.maxTiles} * 8 * sizeof(IndirectDraw), BufferUsage::Indirect,
                             "LOD draws (per frame)"));

    const auto vertex = rhi::createShaderFromFile(device, "render/lod.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("LOD renderer");
    }
    const auto fragment = rhi::createShaderFromFile(device, "render/lod.frag", desc.shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("LOD renderer");
    }
    rhi::PipelineDesc pipeline{
        .vertex = vertex.value(), .fragment = fragment.value(), .debugName = "LOD terrain"};
    pipeline.colorFormats[0] = desc.colorFormat;
    pipeline.colorCount = 1;
    pipeline.depthFormat = desc.depthFormat;
    // Strictly in front: where far terrain lies exactly on a chunk surface
    // (drawn first), the chunk wins.
    pipeline.depth.compare = rhi::CompareOp::Greater;
    auto built = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    if (!built) {
        return built.error().withContext("LOD renderer");
    }
    r.m_pipeline = built.value();
    r.registerStats();
    return renderer;
}

void LodRenderer::registerStats() {
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
        .tiles = add(
            {.name = "Render.Lod.Tiles", .description = "LOD tiles on the GPU.", .type = StatType::Gauge}),
        .quads = add(
            {.name = "Render.Lod.Quads", .description = "Quads of all LOD tiles.", .type = StatType::Gauge}),
        .arenaBytes = add({.name = "Render.Lod.ArenaBytes",
                           .description = "LOD vertex arena bytes in use.",
                           .type = StatType::Gauge,
                           .unit = StatUnit::Bytes}),
        .drawnTiles = add({.name = "Render.Lod.DrawnTiles",
                           .description = "LOD tiles drawn this frame (in view, not under real chunks).",
                           .type = StatType::Gauge}),
        .drawnQuads = add({.name = "Render.Lod.DrawnQuads",
                           .description = "Quads of the LOD tiles drawn this frame (two triangles each).",
                           .type = StatType::Gauge}),
        .coveredTiles = add({.name = "Render.Lod.CoveredTiles",
                             .description = "LOD tiles skipped because real chunks cover all of them.",
                             .type = StatType::Gauge}),
        .uploadBytes = add({.name = "Render.Lod.UploadBytes",
                            .description = "LOD mesh bytes uploaded this frame.",
                            .unit = StatUnit::Bytes}),
        .overBudget = add({.name = "Render.Lod.UploadsOverBudget",
                           .description = "LOD uploads put off to the next frame by the budget."}),
        .outOfMemory =
            add({.name = "Render.Lod.UploadsOutOfMemory",
                 .description = "LOD uploads refused because the arena or the tile slots were full."}),
    };
}

LodRenderer::~LodRenderer() {
    m_staging.destroy(m_device);
    m_tileInfo.destroy(m_device);
    m_draws.destroy(m_device);
    if (m_pipeline.isValid()) {
        m_device.destroy(m_pipeline);
    }
    if (m_coverage.isValid()) {
        m_device.destroy(m_coverage);
        m_device.destroy(m_noTextures);
    }
    if (m_coverageSampler.isValid()) {
        m_device.destroy(m_coverageSampler);
    }
    for (const rhi::BufferHandle buffer : {m_arena, m_indices, m_materials, m_params}) {
        if (buffer.isValid()) {
            m_device.destroy(buffer);
        }
    }
}

void LodRenderer::setViewSize(u32 width, u32 height) noexcept {
    m_viewWidth = std::max(width, 1U);
    m_viewHeight = std::max(height, 1U);
}

void LodRenderer::setTextures(rhi::TextureHandle textures, rhi::SamplerHandle sampler,
                              u32 layerCount) noexcept {
    m_textures = textures;
    m_textureSampler = sampler;
    m_textureLayers = layerCount;
}

void LodRenderer::setMaterialColors(std::span<const u32> colors) {
    const usize count = std::min(colors.size(), kLodMaterialCount);
    static_cast<void>(m_device.updateBuffer(m_materials, 0, std::as_bytes(colors.first(count))));
}

void LodRenderer::setCovered(i32 chunkX, i32 chunkZ, bool covered) noexcept {
    setCoverage(chunkX, chunkZ, covered ? kCoverageFull : 0);
}

void LodRenderer::setCoveredBelow(i32 chunkX, i32 chunkZ, i32 topChunkY) noexcept {
    // Stays clear of 0 and full; lod.frag undoes the offset. The offset fits
    // the tallest world preset, and a taller column just draws a little more.
    setCoverage(chunkX, chunkZ, static_cast<u8>(std::clamp(topChunkY + kCoverageLayerOffset, 1, 254)));
}

void LodRenderer::setCoverage(i32 chunkX, i32 chunkZ, u8 value) noexcept {
    // Two's complement masking wraps negative columns too, like the shader.
    const auto index = (static_cast<usize>(chunkZ & (kLodCoverageSize - 1)) * kLodCoverageSize) +
                       static_cast<usize>(chunkX & (kLodCoverageSize - 1));
    if (m_coverageBytes[index] != value) {
        m_coverageBytes[index] = value;
        m_coverageDirty = true;
        ++m_coverageVersion;
    }
}

bool LodRenderer::isCovered(i32 chunkX, i32 chunkZ) const noexcept {
    return m_coverageBytes[(static_cast<usize>(chunkZ & (kLodCoverageSize - 1)) * kLodCoverageSize) +
                           static_cast<usize>(chunkX & (kLodCoverageSize - 1))] == kCoverageFull;
}

void LodRenderer::clearCoverage() noexcept {
    std::ranges::fill(m_coverageBytes, u8{0});
    m_coverageDirty = true;
    ++m_coverageVersion;
}

// True if every chunk column under the tile is covered. Only tiles that fit
// in the coverage map can be (the map wraps, so a bigger tile would see
// columns twice). Cached until the coverage changes.
bool LodRenderer::fullyCovered(Tile& tile) const {
    if (!m_coverageDiscard.get() || m_showCoverage.get()) {
        return false;
    }
    if (tile.coveredAt == m_coverageVersion) {
        return tile.fullyCovered;
    }
    tile.coveredAt = m_coverageVersion;
    tile.fullyCovered = false;
    const i64 columns = (i64{tile.cellSize} * 64) / kChunkSize;
    if (columns > kLodCoverageSize / 2) {
        return false;
    }
    const i64 firstX = tile.originX >> kChunkShift;
    const i64 firstZ = tile.originZ >> kChunkShift;
    for (i64 z = firstZ; z < firstZ + columns; ++z) {
        for (i64 x = firstX; x < firstX + columns; ++x) {
            if (!isCovered(static_cast<i32>(x), static_cast<i32>(z))) {
                return false;
            }
        }
    }
    tile.fullyCovered = true;
    return true;
}

void LodRenderer::beginFrame(const Camera& camera) {
    VXE_PROFILE_FUNCTION();
    ++m_frame;
    m_camera = camera;
    m_stagingMemory = m_staging.next(m_device);
    m_stagingUsed = 0;
    m_uploadedBytes = 0;
    m_pendingCopies.clear();
    m_drawsBuilt = false;
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
    if (m_coverageDirty) {
        m_coverageDirty = false;
        static_cast<void>(
            m_device.uploadTexture(m_coverage, {.data = std::as_bytes(std::span(m_coverageBytes))}));
    }
    // 0: draw everywhere, 1: hide over covered columns, 2: show them in magenta.
    f32 coverage = 0.0F;
    if (m_coverageDiscard.get()) {
        coverage = m_showCoverage.get() ? 2.0F : 1.0F;
    }
    // w: bit 0 color by level, bit 1 face shading, bit 2 textures, the rest the
    // count of texture layers. A float holds all of it exactly.
    const bool textured = m_textures.isValid() && m_textureSampler.isValid();
    const f32 flags = (m_colorLevels.get() ? 1.0F : 0.0F) + (m_faceShading > 0.5F ? 2.0F : 0.0F) +
                      (textured ? 4.0F + static_cast<f32>(m_textureLayers * 8U) : 0.0F);
    const LodParams params{.params = Vec4(m_skyLight.x, m_brightness, coverage, flags),
                           .skyLight = Vec4(m_skyLight, 1.0F)};
    if (params.params != m_lastParams || params.skyLight != m_lastSkyLight) {
        m_lastParams = params.params;
        m_lastSkyLight = params.skyLight;
        static_cast<void>(m_device.updateBuffer(m_params, 0, std::as_bytes(std::span(&params, 1))));
    }
}

LodRenderer::UploadResult LodRenderer::upload(const LodGeometry& geometry) {
    VXE_ASSERT(!m_stagingMemory.empty(), "Call beginFrame before uploading");
    VXE_ASSERT(geometry.quads.size() % kLodQuadBytes == 0, "LOD quads are 12 bytes each");
    const usize bytes = geometry.quads.size();
    if (bytes == 0) {
        remove(geometry.id);
        return UploadResult::Uploaded;
    }
    // One tile may pass the budget when nothing was uploaded yet this frame.
    const usize budget =
        std::min(static_cast<usize>(std::max(m_uploadBudgetKb.get(), 1)) * 1024, m_stagingMemory.size());
    if (m_stagingUsed + bytes > budget && (m_stagingUsed > 0 || bytes > m_stagingMemory.size())) {
        if (m_stats != nullptr && m_statIds.overBudget.isValid()) {
            m_stats->add(m_statIds.overBudget);
        }
        return UploadResult::OverBudget;
    }
    const auto quads = static_cast<u32>(bytes / kLodQuadBytes);
    const auto existing = m_tiles.find(geometry.id);
    const bool replacing = existing != m_tiles.end();
    const OffsetAllocator::Allocation range = m_allocator.allocate(quads);
    if (!range.isValid() || (!replacing && m_freeSlots.empty())) {
        m_allocator.free(range);
        if (m_stats != nullptr && m_statIds.outOfMemory.isValid()) {
            m_stats->add(m_statIds.outOfMemory);
        }
        return UploadResult::OutOfMemory;
    }
    std::memcpy(m_stagingMemory.data() + m_stagingUsed, geometry.quads.data(), bytes);
    m_pendingCopies.push_back(
        {.stagingOffset = m_stagingUsed, .arenaOffset = usize{range.offset} * kLodQuadBytes, .bytes = bytes});
    m_stagingUsed += bytes;
    m_uploadedBytes += bytes;

    Tile tile{.range = range,
              .originX = geometry.originX,
              .originZ = geometry.originZ,
              .cellSize = geometry.cellSize,
              .level = geometry.level,
              .baseY = geometry.baseY,
              .minY = geometry.minY,
              .maxY = geometry.maxY};
    if (replacing) {
        // Frames in flight may still draw the old range.
        m_quads -= m_allocator.sizeOf(existing->second.range);
        m_deferred.push_back(
            {.frame = m_frame, .range = existing->second.range, .slot = OffsetAllocator::kNoSpace});
        tile.slot = existing->second.slot;
        existing->second = tile;
    } else {
        tile.slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_tiles.emplace(geometry.id, tile);
    }
    m_quads += quads;
    return UploadResult::Uploaded;
}

bool LodRenderer::remove(u64 id) {
    const auto it = m_tiles.find(id);
    if (it == m_tiles.end()) {
        return false;
    }
    release(it->second);
    m_tiles.erase(it);
    return true;
}

void LodRenderer::clear() {
    for (const auto& [id, tile] : m_tiles) {
        release(tile);
    }
    m_tiles.clear();
    clearCoverage();
}

void LodRenderer::drawDebug(DebugDraw& draw) const {
    if (!m_tileBorders.get()) {
        return;
    }
    // Level 1 red through level 6 violet, as Debug.LOD.ColorLevels.
    constexpr std::array<u32, 6> kColors = {debugColor(255, 77, 77),  debugColor(255, 178, 51),
                                            debugColor(230, 255, 77), debugColor(77, 255, 102),
                                            debugColor(77, 178, 255), debugColor(178, 102, 255)};
    for (const auto& [id, tile] : m_tiles) {
        const auto size = static_cast<f64>(tile.cellSize) * 64.0;
        const auto x = static_cast<f64>(tile.originX);
        const auto z = static_cast<f64>(tile.originZ);
        const u32 color = kColors.at(std::clamp<u32>(tile.level, 1, 6) - 1);
        draw.box(DVec3(x, static_cast<f64>(tile.minY), z),
                 DVec3(x + size, static_cast<f64>(tile.maxY), z + size), color);
    }
}

void LodRenderer::release(const Tile& tile) {
    m_quads -= m_allocator.sizeOf(tile.range);
    m_deferred.push_back({.frame = m_frame, .range = tile.range, .slot = tile.slot});
}

void LodRenderer::recordUploads(rhi::CommandList& commands) {
    VXE_PROFILE_FUNCTION();
    for (const PendingCopy& copy : m_pendingCopies) {
        commands.copyBuffer(m_staging.current(), copy.stagingOffset, m_arena, copy.arenaOffset, copy.bytes);
    }
    if (!m_pendingCopies.empty()) {
        commands.resourceBarrier(rhi::BarrierScope::StorageData);
    }
    buildDraws();
    if (m_stats != nullptr) {
        const auto set = [this](stats::StatId id, f64 value) {
            if (id.isValid()) {
                m_stats->set(id, value);
            }
        };
        set(m_statIds.tiles, static_cast<f64>(m_tiles.size()));
        set(m_statIds.quads, static_cast<f64>(m_quads));
        set(m_statIds.arenaBytes, static_cast<f64>(m_quads * kLodQuadBytes));
        set(m_statIds.drawnTiles, static_cast<f64>(m_drawnTiles));
        set(m_statIds.coveredTiles, static_cast<f64>(m_coveredTiles));
        set(m_statIds.drawnQuads, static_cast<f64>(m_drawnQuads));
        if (m_statIds.uploadBytes.isValid() && m_uploadedBytes > 0) {
            m_stats->add(m_statIds.uploadBytes, static_cast<i64>(m_uploadedBytes));
        }
    }
}

// Writes this frame's tile positions (relative to the camera's chunk, with
// integer math) and one indirect draw per visible tile (more for tiles over
// kMaxQuadsPerDraw quads). Tiles outside the view are skipped.
void LodRenderer::buildDraws() {
    VXE_PROFILE_FUNCTION();
    m_drawsBuilt = true;
    m_drawCount = 0;
    m_drawnTiles = 0;
    m_coveredTiles = 0;
    m_drawnQuads = 0;
    if (m_tiles.empty()) {
        return;
    }
    const std::span<std::byte> infoMemory = m_tileInfo.next(m_device);
    const std::span<std::byte> drawMemory = m_draws.next(m_device);
    const usize maxDraws = drawMemory.size() / sizeof(IndirectDraw);
    const CameraPosition camera = m_camera.renderPosition();
    const f32 aspect = static_cast<f32>(m_viewWidth) / static_cast<f32>(m_viewHeight);
    const Frustum frustum = Frustum::fromViewProjection(m_camera.viewProjection(aspect));
    const i64 cameraX = i64{camera.chunk.x} * kChunkSize;
    const i64 cameraY = i64{camera.chunk.y} * kChunkSize;
    const i64 cameraZ = i64{camera.chunk.z} * kChunkSize;
    const auto clampI32 = [](i64 value) {
        return static_cast<i32>(
            std::clamp<i64>(value, std::numeric_limits<i32>::min() / 2, std::numeric_limits<i32>::max() / 2));
    };
    for (auto& [id, tile] : m_tiles) {
        // The map repeats, so a tile whole map widths away reads the coverage of
        // the columns near the camera. Real chunks only stand near the camera.
        const i64 column = (tile.originX >> kChunkShift) - camera.chunk.x;
        const i64 row = (tile.originZ >> kChunkShift) - camera.chunk.z;
        const i64 across = (i64{tile.cellSize} * 64) / kChunkSize;
        const bool inMap = column > -kLodCoverageSize / 2 && column + across <= kLodCoverageSize / 2 &&
                           row > -kLodCoverageSize / 2 && row + across <= kLodCoverageSize / 2;
        if (inMap && fullyCovered(tile)) {
            ++m_coveredTiles;
            continue;
        }
        const glm::ivec4 origin(clampI32(tile.originX - cameraX), clampI32(i64{tile.baseY} - cameraY),
                                clampI32(tile.originZ - cameraZ), static_cast<i32>(tile.cellSize));
        const Vec3 boxMin = Vec3(static_cast<f32>(origin.x), static_cast<f32>(tile.minY - cameraY),
                                 static_cast<f32>(origin.z)) -
                            camera.fraction;
        const f32 size = static_cast<f32>(tile.cellSize) * static_cast<f32>(64);
        const Vec3 boxMax =
            Vec3(boxMin.x + size, static_cast<f32>(tile.maxY - cameraY) - camera.fraction.y, boxMin.z + size);
        if (!frustum.intersects(Aabb{.min = boxMin, .max = boxMax})) {
            continue;
        }
        const TileInfo info{.origin = origin};
        std::memcpy(infoMemory.data() + (usize{tile.slot} * sizeof(TileInfo)), &info, sizeof(info));
        ++m_drawnTiles;
        const u32 quads = m_allocator.sizeOf(tile.range);
        m_drawnQuads += quads;
        for (u32 first = 0; first < quads && m_drawCount < maxDraws; first += kMaxQuadsPerDraw) {
            const u32 count = std::min(quads - first, kMaxQuadsPerDraw);
            const IndirectDraw draw{.indexCount = count * kIndicesPerQuad,
                                    .instanceCount = 1,
                                    .firstIndex = 0,
                                    .baseVertex = static_cast<i32>((tile.range.offset + first) * 4),
                                    .baseInstance = tile.slot};
            std::memcpy(drawMemory.data() + (usize{m_drawCount} * sizeof(IndirectDraw)), &draw, sizeof(draw));
            ++m_drawCount;
        }
    }
}

void LodRenderer::record(rhi::CommandList& commands) {
    if (!m_drawsBuilt || m_drawCount == 0) {
        return;
    }
    commands.bindPipeline(m_pipeline);
    commands.bindUniformBuffer(1, 0, m_params, 0, sizeof(LodParams));
    commands.bindStorageBuffer(2, 0, m_arena, 0, m_arenaBytes);
    commands.bindStorageBuffer(2, 1, m_tileInfo.current(), 0, usize{m_maxTiles} * sizeof(TileInfo));
    commands.bindStorageBuffer(2, 2, m_materials, 0, kLodMaterialCount * sizeof(u32));
    commands.bindTexture(2, 3, m_coverage, m_coverageSampler);
    if (m_textures.isValid() && m_textureSampler.isValid()) {
        commands.bindTexture(2, 4, m_textures, m_textureSampler);
    } else {
        // Nothing reads it then, but every binding needs a texture.
        commands.bindTexture(2, 4, m_noTextures, m_coverageSampler);
    }
    commands.bindIndexBuffer(m_indices, rhi::IndexType::Uint16);
    commands.multiDrawIndexedIndirect(m_draws.current(), 0, m_drawCount);
}

} // namespace vxe::render
