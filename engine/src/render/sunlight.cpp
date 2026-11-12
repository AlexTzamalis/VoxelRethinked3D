#include "vxe/render/sunlight.hpp"

#include <vxe/core/profile.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe::render {
namespace {

// The layout must match TerrainShadowParams in render/terrain_shadow.comp.
struct TerrainShadowParams {
    Vec4 toLight{0.0F};
    Vec4 grid{0.0F}; // Cells, blocks per cell, highest land, lowest known height.
};

constexpr u32 kGroupSize = 8;              // local_size of render/terrain_shadow.comp.
constexpr f32 kNoHeight = -1.0e8F;         // Below: unknown (voxel::kNoLodHeight is -1e9).
constexpr f32 kRecomputeCosine = 0.99998F; // About 0.36 degrees of turn.
constexpr f32 kCloudStrength = 0.55F;      // Share of direct light a thick cloud takes.
constexpr f64 kCloudMapBlocks = f64{kCloudMapSize} * kCloudCellBlocks;

} // namespace

SunlightShadows::SunlightShadows(const SunlightDesc& desc)
    : m_device(*desc.device)
    , m_terrainEnabled(desc.settings->registerBool(
          {{.id = "Graphics.Shadows.Terrain",
            .displayName = "Mountain shadows",
            .description = "Mountains and far terrain shade the land behind them from the sun, as far as the "
                           "far terrain reaches."},
           true}))
    , m_cloudsEnabled(desc.settings->registerBool(
          {{.id = "Graphics.Shadows.Clouds",
            .displayName = "Cloud shadows",
            .description = "Clouds shade the ground below them, and the shade drifts with the wind."},
           true})) {}

Result<std::unique_ptr<SunlightShadows>> SunlightShadows::create(const SunlightDesc& desc) {
    std::unique_ptr<SunlightShadows> sunlight(new SunlightShadows(desc));
    SunlightShadows& s = *sunlight;
    rhi::Device& device = *desc.device;
    const auto fail = [](const Error& error) { return error.withContext("Long shadows"); };
    const auto shader = rhi::createShaderFromFile(device, "render/terrain_shadow.comp", desc.shaders);
    if (!shader) {
        return fail(shader.error());
    }
    auto pipeline = device.createPipeline({.compute = shader.value(), .debugName = "Terrain shadow"});
    device.destroy(shader.value());
    if (!pipeline) {
        return fail(pipeline.error());
    }
    s.m_compute = pipeline.value();
    auto heights = device.createTexture({.format = rhi::Format::R32Float,
                                         .width = kTerrainShadowCells,
                                         .height = kTerrainShadowCells,
                                         .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                         .debugName = "Terrain heights"});
    if (!heights) {
        return fail(heights.error());
    }
    s.m_heights = heights.value();
    auto tops = device.createTexture({.format = rhi::Format::R32Float,
                                      .width = kTerrainShadowCells,
                                      .height = kTerrainShadowCells,
                                      .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled,
                                      .debugName = "Terrain shadow tops"});
    if (!tops) {
        return fail(tops.error());
    }
    s.m_shadowTops = tops.value();
    auto nearest = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "Terrain heights"});
    auto linear = device.createSampler({.minFilter = rhi::Filter::Linear,
                                        .magFilter = rhi::Filter::Linear,
                                        .mipmapMode = rhi::MipmapMode::None,
                                        .addressU = rhi::AddressMode::ClampToEdge,
                                        .addressV = rhi::AddressMode::ClampToEdge,
                                        .debugName = "Terrain shadow tops"});
    if (!nearest || !linear) {
        return fail(!nearest ? nearest.error() : linear.error());
    }
    s.m_nearest = nearest.value();
    s.m_linear = linear.value();
    auto params = device.createBuffer({.sizeBytes = sizeof(TerrainShadowParams),
                                       .usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst,
                                       .debugName = "Terrain shadow parameters"});
    if (!params) {
        return fail(params.error());
    }
    s.m_computeParams = params.value();
    VXE_TRY(
        s.m_ring.create(device, sizeof(SunlightParams), rhi::BufferUsage::Uniform, "Sunlight parameters"));
    return sunlight;
}

SunlightShadows::~SunlightShadows() {
    m_device.destroy(m_compute);
    m_device.destroy(m_heights);
    m_device.destroy(m_shadowTops);
    m_device.destroy(m_nearest);
    m_device.destroy(m_linear);
    m_device.destroy(m_computeParams);
    m_ring.destroy(m_device);
}

void SunlightShadows::setTerrainHeights(const TerrainHeightGrid& grid) {
    if (grid.heights.size() != usize{kTerrainShadowCells} * kTerrainShadowCells) {
        return;
    }
    if (const Result<void> uploaded =
            m_device.uploadTexture(m_heights, {.data = std::as_bytes(std::span<const f32>(grid.heights))});
        !uploaded) {
        return;
    }
    m_originX = grid.originX;
    m_originZ = grid.originZ;
    m_highest = kNoHeight;
    m_lowest = 1.0e9F;
    for (const f32 height : grid.heights) {
        if (height > kNoHeight) {
            m_highest = std::max(m_highest, height);
            m_lowest = std::min(m_lowest, height);
        }
    }
    m_hasTerrain = m_highest > kNoHeight;
    m_dirty = true;
}

void SunlightShadows::clearTerrain() {
    m_hasTerrain = false;
    m_dirty = false;
}

void SunlightShadows::update(const Camera& camera, const ShadowLight& light, const CloudShadowInfo& clouds) {
    m_clouds = clouds;
    SunlightParams params;
    params.toLight = Vec4(light.toLight, light.strength);
    const bool terrain = m_terrainEnabled.get() && m_hasTerrain && light.strength > 0.0F;
    if (terrain) {
        params.terrain =
            Vec4(static_cast<f32>(static_cast<f64>(m_originX) - camera.position.x),
                 static_cast<f32>(static_cast<f64>(m_originZ) - camera.position.z),
                 static_cast<f32>(kTerrainShadowCellBlocks), static_cast<f32>(kTerrainShadowCells));
    }
    // A cell is 8 blocks: its shadow top is only that exact, so the land is
    // let off a few blocks, and the edge softens over more as the sun gets
    // low and the shadow tops run long.
    const f32 low = 1.0F - std::clamp(light.toLight.y * 2.0F, 0.0F, 1.0F);
    params.heights =
        Vec4(static_cast<f32>(camera.position.y), 3.0F, 4.0F + (8.0F * low), terrain ? light.strength : 0.0F);
    if (m_cloudsEnabled.get() && clouds.visible) {
        const f64 x = std::fmod(camera.position.x, kCloudMapBlocks) + clouds.windX;
        const f64 z = std::fmod(camera.position.z, kCloudMapBlocks) + clouds.windZ;
        params.cloud =
            Vec4(clouds.floorY, clouds.threshold, kCloudStrength * light.strength, clouds.thickness);
        params.cloudAt = Vec4(static_cast<f32>(x), static_cast<f32>(z), 0.0F, 0.0F);
    }
    m_params = params;
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), &m_params, sizeof(m_params));
    if (terrain && glm::dot(light.toLight, m_computedLight) < kRecomputeCosine) {
        m_dirty = true;
    }
}

void SunlightShadows::recordCompute(rhi::CommandList& commands) {
    if (!m_dirty || !m_hasTerrain || !m_terrainEnabled.get() || m_params.toLight.w <= 0.0F) {
        return;
    }
    VXE_PROFILE_FUNCTION();
    m_dirty = false;
    m_computedLight = Vec3(m_params.toLight);
    const TerrainShadowParams params{.toLight = Vec4(glm::normalize(m_computedLight), 0.0F),
                                     .grid = Vec4(static_cast<f32>(kTerrainShadowCells),
                                                  static_cast<f32>(kTerrainShadowCellBlocks), m_highest,
                                                  m_lowest)};
    static_cast<void>(m_device.updateBuffer(m_computeParams, 0, std::as_bytes(std::span(&params, 1))));
    commands.bindPipeline(m_compute);
    commands.bindUniformBuffer(1, 0, m_computeParams, 0, sizeof(TerrainShadowParams));
    commands.bindTexture(2, 0, m_heights, m_nearest);
    commands.bindStorageImage(1, 1, m_shadowTops, 0, rhi::ImageAccess::WriteOnly);
    const u32 groups = (kTerrainShadowCells + kGroupSize - 1) / kGroupSize;
    commands.dispatch(groups, groups);
    commands.resourceBarrier(rhi::BarrierScope::StorageImage | rhi::BarrierScope::TextureFetch);
}

void SunlightShadows::bind(rhi::CommandList& commands) const {
    commands.bindUniformBuffer(1, 5, m_ring.current(), 0, sizeof(SunlightParams));
    commands.bindTexture(2, 6, m_shadowTops, m_linear);
    // A texture must stay bound even when clouds are off; the shader skips it.
    const bool clouds = m_clouds.map.isValid() && m_clouds.sampler.isValid();
    commands.bindTexture(2, 7, clouds ? m_clouds.map : m_heights, clouds ? m_clouds.sampler : m_nearest);
}

} // namespace vxe::render
