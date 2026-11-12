#include "vxe/render/water.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe::render {
namespace {

// The layout of WaterParams in render/underwater.frag and render/chunk.frag.
struct WaterParams {
    Vec4 fog;     // rgb: the fog's color under water, a: its density per block (0 off).
    Vec4 surface; // x: light swallowed per block behind a surface (0 off), y: share left in the deep.
};

// Fog under water reaches 90% at the clarity distance: 1 - exp(-2.3) = 0.9.
constexpr f32 kFogAtClarity = 2.3F;
// Behind a surface, water swallows light more slowly than the fog closes in:
// a floor stays in sight through a few blocks, the deep goes dark.
constexpr f32 kSurfaceAbsorption = 1.2F;
// How dark the water gets in the deep, of its own color.
constexpr f32 kDeepShare = 0.3F;
// Blocks below the surface over which the fog loses most of the daylight.
constexpr f32 kDarkeningDepth = 48.0F;

} // namespace

WaterRenderer::WaterRenderer(const WaterDesc& desc)
    : m_device(*desc.device)
    , m_depthColor(desc.settings->registerBool(
          {{.id = "Graphics.Water.DepthColor",
            .displayName = "Water depth color",
            .description = "Water is clear over a shallow floor and dark blue where it is deep."},
           true}))
    , m_fog(desc.settings->registerBool(
          {{.id = "Graphics.Water.UnderwaterFog",
            .displayName = "Underwater fog",
            .description = "Under water the view closes in with the water's color, darker the deeper."},
           true}))
    , m_clarity(desc.settings->registerFloat(
          {{.id = "Graphics.Water.Clarity",
            .displayName = "Water clarity",
            .description = "How far you see under water and into it from above, in blocks."},
           24.0F,
           6.0F,
           96.0F,
           1.0F})) {}

Result<std::unique_ptr<WaterRenderer>> WaterRenderer::create(const WaterDesc& desc) {
    std::unique_ptr<WaterRenderer> water(new WaterRenderer(desc));
    rhi::Device& device = *desc.device;
    const auto vertex = rhi::createShaderFromFile(device, "render/sky.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Water");
    }
    const auto copy = rhi::createShaderFromFile(device, "render/water_depth.frag", desc.shaders);
    const auto fog = rhi::createShaderFromFile(device, "render/underwater.frag", desc.shaders);
    if (!copy || !fog) {
        device.destroy(vertex.value());
        if (copy) {
            device.destroy(copy.value());
        }
        if (fog) {
            device.destroy(fog.value());
        }
        return (!copy ? copy.error() : fog.error()).withContext("Water");
    }
    rhi::PipelineDesc pipeline{
        .vertex = vertex.value(), .fragment = copy.value(), .debugName = "Water depth copy"};
    pipeline.raster.cullMode = rhi::CullMode::None;
    pipeline.depth.testEnabled = false;
    pipeline.depth.writeEnabled = false;
    pipeline.colorFormats[0] = rhi::Format::R32Float;
    pipeline.colorCount = 1;
    auto copyPipeline = device.createPipeline(pipeline);
    pipeline.fragment = fog.value();
    pipeline.debugName = "Underwater fog";
    pipeline.colorFormats[0] = desc.colorFormat;
    // The fog's color over the scene by its amount (alpha).
    pipeline.blend[0] = rhi::BlendState{.enabled = true,
                                        .srcColor = rhi::BlendFactor::SrcAlpha,
                                        .dstColor = rhi::BlendFactor::OneMinusSrcAlpha,
                                        .srcAlpha = rhi::BlendFactor::Zero,
                                        .dstAlpha = rhi::BlendFactor::One};
    auto fogPipeline = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(copy.value());
    device.destroy(fog.value());
    if (!copyPipeline || !fogPipeline) {
        if (copyPipeline) {
            device.destroy(copyPipeline.value());
        }
        if (fogPipeline) {
            device.destroy(fogPipeline.value());
        }
        return (!copyPipeline ? copyPipeline.error() : fogPipeline.error()).withContext("Water");
    }
    water->m_copyPipeline = copyPipeline.value();
    water->m_fogPipeline = fogPipeline.value();
    auto nearest = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "Water depth sampler"});
    if (!nearest) {
        return nearest.error().withContext("Water");
    }
    water->m_nearest = nearest.value();
    VXE_TRY(water->m_ring.create(device, sizeof(WaterParams), rhi::BufferUsage::Uniform, "Water parameters"));
    return water;
}

WaterRenderer::~WaterRenderer() {
    m_device.destroy(m_copyPipeline);
    m_device.destroy(m_fogPipeline);
    m_device.destroy(m_nearest);
    m_ring.destroy(m_device);
}

void WaterRenderer::update(const Vec3& skyLight, f32 daylight) {
    const f32 clarity = std::max(m_clarity.get(), 1.0F);
    // Under water the light is the sky's (dim at night) and fades with depth;
    // a little always stays, so the deep is dark blue and not black.
    const f32 fade = std::exp(-std::max(m_view.depth, 0.0F) / kDarkeningDepth);
    const Vec3 lit = m_view.color * glm::max(skyLight, Vec3(0.05F)) * (0.15F + (0.85F * fade)) *
                     (0.35F + (0.65F * std::clamp(daylight, 0.0F, 1.0F)));
    const WaterParams params{
        .fog = Vec4(lit, isFogging() ? kFogAtClarity / clarity : 0.0F),
        .surface = Vec4(m_depthColor.get() ? kSurfaceAbsorption / clarity : 0.0F, kDeepShare, 0.0F, 0.0F)};
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), &params, sizeof(params));
}

void WaterRenderer::recordDepthCopy(rhi::CommandList& commands, rhi::TextureHandle sceneDepth) const {
    commands.bindPipeline(m_copyPipeline);
    commands.bindTexture(2, 0, sceneDepth, m_nearest);
    commands.draw(3);
}

void WaterRenderer::bindSurfaces(rhi::CommandList& commands, rhi::TextureHandle depthCopy) const {
    commands.bindUniformBuffer(1, 4, m_ring.current(), 0, sizeof(WaterParams));
    commands.bindTexture(2, 8, depthCopy, m_nearest);
}

void WaterRenderer::recordFog(rhi::CommandList& commands, rhi::TextureHandle sceneDepth) const {
    if (!isFogging()) {
        return;
    }
    commands.bindPipeline(m_fogPipeline);
    commands.bindUniformBuffer(1, 4, m_ring.current(), 0, sizeof(WaterParams));
    commands.bindTexture(2, 0, sceneDepth, m_nearest);
    commands.draw(3);
}

} // namespace vxe::render
