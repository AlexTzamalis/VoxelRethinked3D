#include "vxe/render/clouds.hpp"

#include <vxe/math/noise.hpp>
#include <vxe/render/sky.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace vxe::render {
namespace {

// The layout must match CloudParams in clouds.frag.
struct CloudParams {
    Vec4 shape;  // Floor height, thickness, density where clouds start, light lost per block.
    Vec4 motion; // Wind x and z in blocks, where they start and end fading.
    Vec4 style;  // x: blockiness, y: steps per ray, z: 1 with detail noise.
};

constexpr std::array<config::EnumSettingDesc<CloudQuality>::Option, 3> kQualities = {
    {{CloudQuality::Low, "Low", "Low"},
     {CloudQuality::Medium, "Medium", "Medium"},
     {CloudQuality::High, "High", "High"}}};
constexpr std::array<f32, 3> kStepsOfQuality = {24.0F, 40.0F, 64.0F};

constexpr f32 kFadeStart = 3500.0F; // Blocks from the camera.
constexpr f32 kFadeEnd = 6000.0F;
constexpr f64 kWindAngle = 0.6; // Radians from +X. Clouds drift one way all day.
constexpr f64 kMapBlocks = static_cast<f64>(kCloudMapSize * kCloudCellBlocks);
constexpr u32 kOctaves = 4;
constexpr u32 kFirstPeriod =
    20; // Noise cells across the map in the largest octave: clouds of about 200 blocks.

// Smooth noise that repeats every `period` lattice cells: the wrap happens on
// the lattice, so the first row of the map continues the last.
f32 wrappedNoise(u64 seed, f32 x, f32 y, u32 period) noexcept {
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const auto ix = static_cast<i64>(fx);
    const auto iy = static_cast<i64>(fy);
    const auto corner = [&](i64 dx, i64 dy) {
        const i64 cx = (ix + dx) % period;
        const i64 cy = (iy + dy) % period;
        return static_cast<f32>(noise::unit(noise::hash2(seed, cx, cy)));
    };
    const f32 tx = x - fx;
    const f32 ty = y - fy;
    const f32 sx = tx * tx * (3.0F - (2.0F * tx));
    const f32 sy = ty * ty * (3.0F - (2.0F * ty));
    const f32 top = corner(0, 0) + ((corner(1, 0) - corner(0, 0)) * sx);
    const f32 bottom = corner(0, 1) + ((corner(1, 1) - corner(0, 1)) * sx);
    return top + ((bottom - top) * sy);
}

} // namespace

std::array<std::vector<u8>, kCloudMapLevels> buildCloudMap(u64 seed) {
    constexpr auto kSize = static_cast<usize>(kCloudMapSize);
    std::vector<f32> values(kSize * kSize);
    for (usize y = 0; y < kSize; ++y) {
        for (usize x = 0; x < kSize; ++x) {
            f32 sum = 0.0F;
            f32 amplitude = 1.0F;
            u32 period = kFirstPeriod;
            for (u32 octave = 0; octave < kOctaves; ++octave) {
                const f32 scale = static_cast<f32>(period) / static_cast<f32>(kSize);
                sum += wrappedNoise(seed + octave, static_cast<f32>(x) * scale, static_cast<f32>(y) * scale,
                                    period) *
                       amplitude;
                amplitude *= 0.5F;
                period *= 2;
            }
            values[(y * kSize) + x] = sum;
        }
    }
    // Ranks instead of raw noise: noise piles up around its middle, and a cover
    // setting should mean the share of sky it says.
    std::vector<f32> sorted = values;
    std::ranges::sort(sorted);
    std::array<std::vector<u8>, kCloudMapLevels> levels;
    levels[0].resize(values.size());
    for (usize i = 0; i < values.size(); ++i) {
        const auto rank = static_cast<f32>(std::ranges::lower_bound(sorted, values[i]) - sorted.begin());
        levels[0][i] = static_cast<u8>(std::lround(rank / static_cast<f32>(values.size() - 1) * 255.0F));
    }
    // Each level holds the average of four cells below it.
    for (usize level = 1; level < kCloudMapLevels; ++level) {
        const usize size = kSize >> level;
        levels.at(level).resize(size * size);
        const std::vector<u8>& finer = levels.at(level - 1);
        for (usize y = 0; y < size; ++y) {
            for (usize x = 0; x < size; ++x) {
                const usize row = y * 2 * size * 2;
                const u32 sum = u32{finer[row + (x * 2)]} + finer[row + (x * 2) + 1] +
                                finer[row + (size * 2) + (x * 2)] + finer[row + (size * 2) + (x * 2) + 1];
                levels.at(level)[(y * size) + x] = static_cast<u8>((sum + 2) / 4);
            }
        }
    }
    return levels;
}

CloudRenderer::CloudRenderer(const CloudDesc& desc)
    : m_device(*desc.device) {
    config::SettingsRegistry& settings = *desc.settings;
    const auto number = [&settings](std::string_view id, std::string_view name, std::string_view description,
                                    f32 value, f32 min, f32 max, f32 step) {
        return settings.registerFloat(
            {{.id = id, .displayName = name, .description = description}, value, min, max, step});
    };
    m_enabled = settings.registerBool({{.id = "Graphics.Clouds.Enabled",
                                        .displayName = "Clouds",
                                        .description = "Draws blocky volumetric clouds."},
                                       true});
    m_height = number("Graphics.Clouds.Height", "Cloud height", "World height of the cloud layer's floor.",
                      270.0F, 64.0F, 2048.0F, 8.0F);
    m_thickness = number("Graphics.Clouds.Thickness", "Cloud thickness",
                         "How many blocks tall the tallest clouds are.", 56.0F, 8.0F, 256.0F, 4.0F);
    m_coverScale =
        number("Graphics.Clouds.CoverScale", "Cloud amount",
               "Times the clouds the weather brings. 1 is as the weather says.", 1.0F, 0.0F, 2.0F, 0.05F);
    m_softness = number("Graphics.Clouds.Softness", "Cloud softness",
                        "How far light gets into a cloud. More is wispier, less is more solid.", 0.5F, 0.05F,
                        1.0F, 0.05F);
    m_blockiness = number("Graphics.Clouds.Blockiness", "Cloud blockiness",
                          "0 is soft and round, 1 is built from blocks.", 0.2F, 0.0F, 1.0F, 0.05F);
    config::EnumSettingDesc<CloudQuality> quality;
    quality.id = "Graphics.Clouds.Quality";
    quality.displayName = "Cloud quality";
    quality.description = "More looks smoother and costs more.";
    quality.defaultValue = CloudQuality::Medium;
    quality.options = kQualities;
    m_quality = settings.registerEnum(quality);
    m_speed = number("Graphics.Clouds.Speed", "Cloud speed",
                     "How fast the wind moves clouds in a 20 minute day, in blocks per second.", 3.0F, 0.0F,
                     64.0F, 0.5F);
}

Result<std::unique_ptr<CloudRenderer>> CloudRenderer::create(const CloudDesc& desc) {
    std::unique_ptr<CloudRenderer> clouds(new CloudRenderer(desc));
    rhi::Device& device = *desc.device;
    const auto vertex = rhi::createShaderFromFile(device, "render/sky.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Clouds");
    }
    const auto fragment = rhi::createShaderFromFile(device, "render/clouds.frag", desc.shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("Clouds");
    }
    rhi::PipelineDesc pipeline{.vertex = vertex.value(), .fragment = fragment.value(), .debugName = "Clouds"};
    pipeline.raster.cullMode = rhi::CullMode::None;
    pipeline.depth.testEnabled = false;
    pipeline.depth.writeEnabled = false;
    pipeline.colorFormats[0] = desc.colorFormat;
    pipeline.colorCount = 1;
    auto created = device.createPipeline(pipeline);
    device.destroy(fragment.value());
    if (!created) {
        device.destroy(vertex.value());
        return created.error().withContext("Clouds");
    }
    clouds->m_pipeline = created.value();

    const auto blendFragment = rhi::createShaderFromFile(device, "render/clouds_blit.frag", desc.shaders);
    if (!blendFragment) {
        device.destroy(vertex.value());
        return blendFragment.error().withContext("Clouds");
    }
    pipeline.fragment = blendFragment.value();
    pipeline.debugName = "Clouds blend";
    pipeline.blend[0] = rhi::BlendState{.enabled = true,
                                        .srcColor = rhi::BlendFactor::One,
                                        .dstColor = rhi::BlendFactor::OneMinusSrcAlpha,
                                        .srcAlpha = rhi::BlendFactor::One,
                                        .dstAlpha = rhi::BlendFactor::OneMinusSrcAlpha};
    auto blended = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(blendFragment.value());
    if (!blended) {
        return blended.error().withContext("Clouds");
    }
    clouds->m_blendPipeline = blended.value();
    auto mapSampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                            .magFilter = rhi::Filter::Linear,
                                            .mipmapMode = rhi::MipmapMode::Linear,
                                            .addressU = rhi::AddressMode::Repeat,
                                            .addressV = rhi::AddressMode::Repeat,
                                            .debugName = "Cloud map sampler"});
    if (!mapSampler) {
        return mapSampler.error().withContext("Clouds");
    }
    clouds->m_mapSampler = mapSampler.value();
    auto layerSampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                              .magFilter = rhi::Filter::Linear,
                                              .mipmapMode = rhi::MipmapMode::None,
                                              .addressU = rhi::AddressMode::ClampToEdge,
                                              .addressV = rhi::AddressMode::ClampToEdge,
                                              .debugName = "Cloud layer sampler"});
    if (!layerSampler) {
        return layerSampler.error().withContext("Clouds");
    }
    clouds->m_layerSampler = layerSampler.value();
    auto noise = device.createTexture({.format = rhi::Format::RGBA8Unorm,
                                       .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                       .debugName = "Cloud noise"});
    if (!noise) {
        return noise.error().withContext("Clouds");
    }
    clouds->m_noise = noise.value();
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "Cloud sampler"});
    if (!sampler) {
        return sampler.error().withContext("Clouds");
    }
    clouds->m_sampler = sampler.value();
    auto map = device.createTexture({.format = rhi::Format::R8Unorm,
                                     .width = kCloudMapSize,
                                     .height = kCloudMapSize,
                                     .mipLevels = kCloudMapLevels,
                                     .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                     .debugName = "Cloud map"});
    if (!map) {
        return map.error().withContext("Clouds");
    }
    clouds->m_map = map.value();
    VXE_TRY(
        clouds->m_ring.create(device, sizeof(CloudParams), rhi::BufferUsage::Uniform, "Cloud parameters"));
    clouds->setSeed(0);
    return clouds;
}

CloudRenderer::~CloudRenderer() {
    m_device.destroy(m_pipeline);
    m_device.destroy(m_blendPipeline);
    m_device.destroy(m_sampler);
    m_device.destroy(m_mapSampler);
    m_device.destroy(m_layerSampler);
    m_device.destroy(m_map);
    m_device.destroy(m_noise);
    m_ring.destroy(m_device);
}

void CloudRenderer::setSeed(u64 seed) {
    const auto levels = buildCloudMap(seed);
    for (u32 level = 0; level < kCloudMapLevels; ++level) {
        static_cast<void>(m_device.uploadTexture(
            m_map, {.mipLevel = level, .data = std::as_bytes(std::span(levels.at(level)))}));
    }
}

void CloudRenderer::setNoise(const PackImage* image) {
    if (image == nullptr || image->width == 0 || image->height == 0) {
        m_noiseLoaded = false;
        return;
    }
    auto texture =
        m_device.createTexture({.format = rhi::Format::RGBA8Unorm,
                                .width = image->width,
                                .height = image->height,
                                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                .debugName = "Cloud noise"});
    if (!texture) {
        m_noiseLoaded = false;
        return;
    }
    m_device.destroy(m_noise);
    m_noise = texture.value();
    static_cast<void>(m_device.uploadTexture(m_noise, {.data = std::as_bytes(std::span(image->pixels))}));
    m_noiseLoaded = true;
}

void CloudRenderer::update(f64 deltaSeconds, f32 cover, f32 timeSpeed) {
    m_cover = std::clamp(cover * m_coverScale.get(), 0.0F, 1.0F);
    const f64 moved = static_cast<f64>(m_speed.get() * timeSpeed) * deltaSeconds;
    m_windX = std::fmod(m_windX + (moved * std::cos(kWindAngle)), kMapBlocks);
    m_windZ = std::fmod(m_windZ + (moved * std::sin(kWindAngle)), kMapBlocks);
    const CloudParams params{
        .shape = Vec4(m_height.get(), m_thickness.get(), 1.0F - m_cover,
                      // Softness 0.5 lets light 40 blocks in; less is denser.
                      0.0125F / m_softness.get()),
        .motion = Vec4(static_cast<f32>(m_windX), static_cast<f32>(m_windZ), kFadeStart, kFadeEnd),
        .style = Vec4(m_blockiness.get(), kStepsOfQuality.at(static_cast<usize>(m_quality.get())),
                      m_noiseLoaded ? 1.0F : 0.0F, 0.0F)};
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), &params, sizeof(params));
}

void CloudRenderer::recordLayer(rhi::CommandList& commands, rhi::TextureHandle sceneDepth,
                                const SkyRenderer& sky) const {
    if (!m_enabled.get() || m_cover <= 0.0F) {
        return;
    }
    commands.bindPipeline(m_pipeline);
    sky.bindParams(commands, 0);
    commands.bindUniformBuffer(1, 1, m_ring.current(), 0, sizeof(CloudParams));
    commands.bindTexture(2, 0, sceneDepth, m_sampler);
    commands.bindTexture(2, 1, m_map, m_mapSampler);
    commands.bindTexture(2, 2, m_noise, m_mapSampler);
    commands.draw(3);
}

CloudShadowInfo CloudRenderer::shadowInfo() const noexcept {
    return {.map = m_map,
            .sampler = m_mapSampler,
            .floorY = m_height.get(),
            .thickness = m_thickness.get(),
            .threshold = 1.0F - m_cover,
            .windX = m_windX,
            .windZ = m_windZ,
            .visible = m_enabled.get() && m_cover > 0.0F};
}

void CloudRenderer::recordBlend(rhi::CommandList& commands, rhi::TextureHandle layer) const {
    if (!m_enabled.get() || m_cover <= 0.0F) {
        return;
    }
    commands.bindPipeline(m_blendPipeline);
    commands.bindTexture(2, 0, layer, m_layerSampler);
    commands.draw(3);
}

} // namespace vxe::render
