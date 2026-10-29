#include "vxe/render/post_process.hpp"

#include <glm/common.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vxe::render {
namespace {

// The block PostParams of post.frag (std140).
struct PostParams {
    Vec4 settings{0.0F}; // exposure, tonemapper, FXAA, unused.
    Vec4 texel{0.0F};
};
static_assert(sizeof(PostParams) == 32);

using CurveOption = config::EnumSettingDesc<Tonemapper>::Option;
constexpr std::array<CurveOption, 3> kCurves{{
    {.value = Tonemapper::None, .name = "None", .displayName = "None (clip)"},
    {.value = Tonemapper::Reinhard, .name = "Reinhard", .displayName = "Reinhard (soft)"},
    {.value = Tonemapper::Aces, .name = "Aces", .displayName = "ACES filmic"},
}};

} // namespace

Vec3 tonemap(Vec3 color, Tonemapper curve, f32 exposure) noexcept {
    color *= exposure;
    switch (curve) {
    case Tonemapper::Reinhard:
        return color / (Vec3(1.0F) + color);
    case Tonemapper::Aces:
        color *= 0.6F; // The fit's reference exposure, as in post.frag.
        return glm::clamp((color * (2.51F * color + 0.03F)) / (color * (2.43F * color + 0.59F) + 0.14F), 0.0F,
                          1.0F);
    case Tonemapper::None:
        break;
    }
    return glm::clamp(color, 0.0F, 1.0F);
}

f32 encodeSrgb(f32 linear) noexcept {
    return linear <= 0.0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

f32 decodeSrgb(f32 encoded) noexcept {
    return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

Vec3 decodeSrgb(Vec3 encoded) noexcept {
    return {decodeSrgb(encoded.r), decodeSrgb(encoded.g), decodeSrgb(encoded.b)};
}

PostProcess::PostProcess(const PostProcessDesc& desc)
    : m_device(*desc.device) {
    config::SettingsRegistry& settings = *desc.settings;
    config::EnumSettingDesc<Tonemapper> curve;
    curve.id = "Graphics.Post.Tonemapper";
    curve.displayName = "Tonemapping";
    curve.description = "How bright light is squeezed into what the screen can show.";
    curve.defaultValue = Tonemapper::Aces;
    curve.options = kCurves;
    m_tonemapper = settings.registerEnum(curve);
    m_exposure = settings.registerFloat({{.id = "Graphics.Post.Exposure",
                                          .displayName = "Exposure",
                                          .description = "Brightness multiplier before tonemapping."},
                                         1.0F,
                                         0.1F,
                                         8.0F,
                                         0.05F});
    m_fxaa = settings.registerBool({{.id = "Graphics.Post.Fxaa",
                                     .displayName = "FXAA",
                                     .description = "Fast approximate anti aliasing: smooths jagged edges."},
                                    true});
}

Result<std::unique_ptr<PostProcess>> PostProcess::create(const PostProcessDesc& desc) {
    std::unique_ptr<PostProcess> post(new PostProcess(desc));
    rhi::Device& device = *desc.device;
    // The blit vertex shader covers the screen with one triangle and passes UVs.
    const auto vertex = rhi::createShaderFromFile(device, "debug/blit.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Post process");
    }
    const auto fragment = rhi::createShaderFromFile(device, "render/post.frag", desc.shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("Post process");
    }
    rhi::PipelineDesc pipeline{
        .vertex = vertex.value(), .fragment = fragment.value(), .debugName = "Post process"};
    pipeline.raster.cullMode = rhi::CullMode::None;
    pipeline.depth.testEnabled = false;
    pipeline.depth.writeEnabled = false;
    pipeline.colorFormats[0] = desc.outputFormat;
    pipeline.colorCount = 1;
    auto created = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    if (!created) {
        return created.error().withContext("Post process");
    }
    post->m_pipeline = created.value();
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                         .magFilter = rhi::Filter::Linear,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "Post process sampler"});
    if (!sampler) {
        return sampler.error().withContext("Post process");
    }
    post->m_sampler = sampler.value();
    VXE_TRY(post->m_ring.create(device, sizeof(PostParams), rhi::BufferUsage::Uniform, "Post parameters"));
    return post;
}

PostProcess::~PostProcess() {
    m_device.destroy(m_pipeline);
    m_device.destroy(m_sampler);
    m_ring.destroy(m_device);
}

void PostProcess::update(u32 sceneWidth, u32 sceneHeight) {
    const PostParams params{
        .settings =
            Vec4(m_exposure.get(), static_cast<f32>(m_tonemapper.get()), m_fxaa.get() ? 1.0F : 0.0F, 0.0F),
        .texel = Vec4(1.0F / static_cast<f32>(std::max(sceneWidth, 1U)),
                      1.0F / static_cast<f32>(std::max(sceneHeight, 1U)), 0.0F, 0.0F),
    };
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), &params, sizeof(params));
}

void PostProcess::record(rhi::CommandList& commands, rhi::TextureHandle hdrScene) const {
    commands.bindPipeline(m_pipeline);
    commands.bindUniformBuffer(1, 0, m_ring.current(), 0, sizeof(PostParams));
    commands.bindTexture(2, 0, hdrScene, m_sampler);
    commands.draw(3);
}

} // namespace vxe::render
