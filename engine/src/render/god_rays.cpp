#include "vxe/render/god_rays.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe::render {
namespace {

// The layout must match GodRayParams in god_rays.frag (std140).
struct GodRayParams {
    Vec4 sun;   // xy: the sun on screen (0 to 1), z: strength.
    Vec4 color; // rgb: the sun's color.
};

f32 smoothstep(f32 low, f32 high, f32 value) noexcept {
    const f32 t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

} // namespace

GodRayView godRayView(const Camera& camera, f32 aspect, const Vec3& toSun) noexcept {
    GodRayView view;
    // A direction (w 0) projects to the point at infinity: where the sun shows.
    const Vec4 clip = camera.viewProjection(aspect) * Vec4(toSun, 0.0F);
    if (clip.w <= 1e-4F) {
        return view;
    }
    view.sunUv = (Vec2(clip.x, clip.y) / clip.w * 0.5F) + 0.5F;
    const f32 outside =
        std::max(std::max(-view.sunUv.x, view.sunUv.x - 1.0F), std::max(-view.sunUv.y, view.sunUv.y - 1.0F));
    const f32 onScreen = 1.0F - smoothstep(0.0F, 0.35F, outside);
    const f32 risen = smoothstep(-0.02F, 0.08F, toSun.y);
    const f32 facing = smoothstep(0.0F, 0.35F, glm::dot(camera.forward(), toSun));
    view.strength = onScreen * risen * facing;
    return view;
}

GodRayRenderer::GodRayRenderer(const GodRayDesc& desc)
    : m_device(*desc.device)
    , m_enabled(
          desc.settings->registerBool({{.id = "Graphics.Sky.GodRays",
                                        .displayName = "Sun rays",
                                        .description = "Shafts of sunlight through leaves, past mountains "
                                                       "and between clouds when looking towards the sun."},
                                       true}))
    , m_strength(desc.settings->registerFloat({{.id = "Graphics.Sky.GodRayStrength",
                                                .displayName = "Sun ray strength",
                                                .description = "How bright the shafts of sunlight are."},
                                               0.6F,
                                               0.0F,
                                               2.0F,
                                               0.05F})) {}

Result<std::unique_ptr<GodRayRenderer>> GodRayRenderer::create(const GodRayDesc& desc) {
    std::unique_ptr<GodRayRenderer> rays(new GodRayRenderer(desc));
    rhi::Device& device = *desc.device;
    const auto vertex = rhi::createShaderFromFile(device, "render/sky.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("God rays");
    }
    const auto fragment = rhi::createShaderFromFile(device, "render/god_rays.frag", desc.shaders);
    const auto blit = rhi::createShaderFromFile(device, "render/god_rays_blit.frag", desc.shaders);
    if (!fragment || !blit) {
        device.destroy(vertex.value());
        if (fragment) {
            device.destroy(fragment.value());
        }
        if (blit) {
            device.destroy(blit.value());
        }
        return (!fragment ? fragment.error() : blit.error()).withContext("God rays");
    }
    rhi::PipelineDesc pipeline{
        .vertex = vertex.value(), .fragment = fragment.value(), .debugName = "God rays"};
    pipeline.raster.cullMode = rhi::CullMode::None;
    pipeline.depth.testEnabled = false;
    pipeline.depth.writeEnabled = false;
    pipeline.colorFormats[0] = desc.colorFormat;
    pipeline.colorCount = 1;
    auto layer = device.createPipeline(pipeline);
    pipeline.fragment = blit.value();
    pipeline.debugName = "God rays blend";
    // Light adds to the scene.
    pipeline.blend[0] = rhi::BlendState{.enabled = true,
                                        .srcColor = rhi::BlendFactor::One,
                                        .dstColor = rhi::BlendFactor::One,
                                        .srcAlpha = rhi::BlendFactor::Zero,
                                        .dstAlpha = rhi::BlendFactor::One};
    auto blend = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    device.destroy(blit.value());
    if (!layer || !blend) {
        if (layer) {
            device.destroy(layer.value());
        }
        if (blend) {
            device.destroy(blend.value());
        }
        return (!layer ? layer.error() : blend.error()).withContext("God rays");
    }
    rays->m_pipeline = layer.value();
    rays->m_blendPipeline = blend.value();
    auto nearest = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "God ray depth sampler"});
    auto linear = device.createSampler({.minFilter = rhi::Filter::Linear,
                                        .magFilter = rhi::Filter::Linear,
                                        .mipmapMode = rhi::MipmapMode::None,
                                        .addressU = rhi::AddressMode::ClampToEdge,
                                        .addressV = rhi::AddressMode::ClampToEdge,
                                        .debugName = "God ray layer sampler"});
    if (!nearest || !linear) {
        return (!nearest ? nearest.error() : linear.error()).withContext("God rays");
    }
    rays->m_nearest = nearest.value();
    rays->m_linear = linear.value();
    VXE_TRY(
        rays->m_ring.create(device, sizeof(GodRayParams), rhi::BufferUsage::Uniform, "God ray parameters"));
    return rays;
}

GodRayRenderer::~GodRayRenderer() {
    m_device.destroy(m_pipeline);
    m_device.destroy(m_blendPipeline);
    m_device.destroy(m_nearest);
    m_device.destroy(m_linear);
    m_ring.destroy(m_device);
}

void GodRayRenderer::update(const Camera& camera, f32 aspect, const Vec3& toSun, const Vec3& sunColor) {
    m_view = godRayView(camera, aspect, toSun);
    const GodRayParams params{.sun = Vec4(m_view.sunUv, m_view.strength * m_strength.get(), 0.0F),
                              .color = Vec4(sunColor, 1.0F)};
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), &params, sizeof(params));
}

bool GodRayRenderer::isShining() const noexcept {
    return m_enabled.get() && m_view.strength * m_strength.get() > 0.001F;
}

void GodRayRenderer::recordLayer(rhi::CommandList& commands, rhi::TextureHandle sceneDepth,
                                 rhi::TextureHandle clouds) const {
    if (!isShining()) {
        return;
    }
    commands.bindPipeline(m_pipeline);
    commands.bindUniformBuffer(1, 0, m_ring.current(), 0, sizeof(GodRayParams));
    commands.bindTexture(2, 0, sceneDepth, m_nearest);
    commands.bindTexture(2, 1, clouds, m_linear);
    commands.draw(3);
}

void GodRayRenderer::recordBlend(rhi::CommandList& commands, rhi::TextureHandle layer) const {
    if (!isShining()) {
        return;
    }
    commands.bindPipeline(m_blendPipeline);
    commands.bindTexture(2, 0, layer, m_linear);
    commands.draw(3);
}

} // namespace vxe::render
