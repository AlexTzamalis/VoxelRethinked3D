#include "vxe/render/sky.hpp"

#include <vxe/render/post_process.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe::render {
namespace {

// The sun's path is tilted this far from straight overhead, so noon light
// comes from the side and terrain gets shape.
constexpr f32 kSunPathTilt = 0.5F; // Radians, about 29 degrees.

} // namespace

SkyParams computeSkyParams(const SkyState& state) noexcept {
    // Colors picked as they look on screen (sRGB). decodeSrgb turns them into
    // the linear values the HDR scene and the tonemapper (E8.4) work with.
    const Vec3 kDayZenith = decodeSrgb(Vec3(0.20F, 0.42F, 0.82F));
    const Vec3 kDayHorizon = decodeSrgb(Vec3(0.66F, 0.80F, 0.95F));
    const Vec3 kDuskZenith = decodeSrgb(Vec3(0.24F, 0.27F, 0.50F));
    const Vec3 kDuskHorizon = decodeSrgb(Vec3(0.96F, 0.56F, 0.32F));
    const Vec3 kNightZenith = decodeSrgb(Vec3(0.012F, 0.016F, 0.045F));
    const Vec3 kNightHorizon = decodeSrgb(Vec3(0.035F, 0.045F, 0.085F));
    const Vec3 kLowSun = decodeSrgb(Vec3(1.0F, 0.55F, 0.25F));
    const Vec3 kHighSun = decodeSrgb(Vec3(1.0F, 0.95F, 0.85F));

    // 6:00 rises in the east (+X), 12:00 is the highest point, 18:00 sets in the west.
    const f32 angle = (state.hours - 6.0F) / 24.0F * glm::two_pi<f32>();
    const Vec3 sun = glm::normalize(Vec3(std::cos(angle), std::sin(angle) * std::cos(kSunPathTilt),
                                         -std::sin(angle) * std::sin(kSunPathTilt)));
    const f32 elevation = sun.y;
    const f32 day = glm::smoothstep(-0.1F, 0.25F, elevation);
    // Dusk colors peak while the sun is near the horizon, and fade out once
    // it is well below it.
    const f32 dusk =
        (1.0F - glm::smoothstep(0.0F, 0.35F, std::abs(elevation))) * glm::smoothstep(-0.2F, 0.0F, elevation);

    SkyParams params;
    params.sunDirection = Vec4(sun, std::cos(glm::radians(state.sunSizeDegrees)));
    params.zenithColor =
        Vec4(glm::mix(glm::mix(kNightZenith, kDayZenith, day), kDuskZenith, dusk * 0.5F), 1.0F);
    params.horizonColor = Vec4(glm::mix(glm::mix(kNightHorizon, kDayHorizon, day), kDuskHorizon, dusk), 1.0F);
    const f32 visible = glm::smoothstep(-0.05F, 0.02F, elevation);
    params.sunColor = Vec4(glm::mix(kLowSun, kHighSun, glm::smoothstep(0.0F, 0.4F, elevation)) * visible,
                           0.35F + 0.65F * dusk);
    params.fog = Vec4(state.fogStart, state.fogDensity, state.fogHeightFalloff, state.fogBaseHeight);
    params.flags = glm::ivec4(state.skyEnabled ? 1 : 0, state.fogEnabled ? 1 : 0, 0, 0);
    return params;
}

SkyRenderer::SkyRenderer(const SkyDesc& desc)
    : m_device(*desc.device)
    , m_stats(desc.stats) {
    config::SettingsRegistry& settings = *desc.settings;
    const auto number = [&settings](std::string_view id, std::string_view name, std::string_view description,
                                    f32 value, f32 min, f32 max, f32 step) {
        return settings.registerFloat(
            {{.id = id, .displayName = name, .description = description}, value, min, max, step});
    };
    m_enabled = settings.registerBool(
        {{.id = "Graphics.Sky.Enabled", .displayName = "Sky", .description = "Draws the sky and the sun."},
         true});
    m_timeOfDay = number("Graphics.Sky.TimeOfDay", "Time of day",
                         "Hours, 0 to 24. Sunrise at 6, sunset at 18.", 10.0F, 0.0F, 24.0F, 0.25F);
    m_dayLength = number("Graphics.Sky.DayLengthMinutes", "Day length",
                         "Real minutes for a full day. 0 stops the sun.", 0.0F, 0.0F, 240.0F, 1.0F);
    m_sunSize = number("Graphics.Sky.SunSize", "Sun size", "Angular radius of the sun in degrees.", 1.2F,
                       0.2F, 6.0F, 0.1F);
    m_fogEnabled = settings.registerBool(
        {{.id = "Graphics.Sky.FogEnabled", .displayName = "Fog", .description = "Distance and height fog."},
         true});
    m_fogStart = number("Graphics.Sky.FogStart", "Fog start", "Blocks from the camera where fog begins.",
                        32.0F, 0.0F, 20000.0F, 8.0F);
    m_fogDensity = number("Graphics.Sky.FogDensity", "Fog density", "How quickly fog thickens with distance.",
                          0.0015F, 0.0F, 0.05F, 0.0005F);
    m_fogFalloff = number("Graphics.Sky.FogHeightFalloff", "Fog height falloff",
                          "How quickly fog thins out above its base height.", 0.02F, 0.0F, 0.5F, 0.005F);
    m_fogBase = number("Graphics.Sky.FogBaseHeight", "Fog base height", "World height where fog is thickest.",
                       64.0F, -4096.0F, 4096.0F, 8.0F);
    if (m_stats != nullptr) {
        const auto gauge = [this](std::string_view name, std::string_view description) {
            auto id = m_stats->registerStat(
                {.name = name, .description = description, .type = stats::StatType::Gauge});
            return id ? id.value() : stats::StatId{};
        };
        m_timeStat = gauge("Render.Sky.TimeOfDay", "Current time of day in hours.");
        m_elevationStat = gauge("Render.Sky.SunElevation", "Sun height above the horizon in degrees.");
    }
}

Result<std::unique_ptr<SkyRenderer>> SkyRenderer::create(const SkyDesc& desc) {
    std::unique_ptr<SkyRenderer> sky(new SkyRenderer(desc));
    rhi::Device& device = *desc.device;
    const auto vertex = rhi::createShaderFromFile(device, "render/sky.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Sky");
    }
    const auto fragment = rhi::createShaderFromFile(device, "render/sky.frag", desc.shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("Sky");
    }
    rhi::PipelineDesc pipeline{
        .vertex = vertex.value(), .fragment = fragment.value(), .debugName = "Sky and fog"};
    pipeline.raster.cullMode = rhi::CullMode::None;
    pipeline.depth.testEnabled = false;
    pipeline.depth.writeEnabled = false;
    pipeline.blend[0] = rhi::BlendState::alphaBlend();
    pipeline.colorFormats[0] = desc.colorFormat;
    pipeline.colorCount = 1;
    auto created = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    if (!created) {
        return created.error().withContext("Sky");
    }
    sky->m_pipeline = created.value();
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "Sky depth sampler"});
    if (!sampler) {
        return sampler.error().withContext("Sky");
    }
    sky->m_sampler = sampler.value();
    VXE_TRY(sky->m_ring.create(device, sizeof(SkyParams), rhi::BufferUsage::Uniform, "Sky parameters"));
    return sky;
}

SkyRenderer::~SkyRenderer() {
    m_device.destroy(m_pipeline);
    m_device.destroy(m_sampler);
    m_ring.destroy(m_device);
}

void SkyRenderer::update(f64 deltaSeconds) {
    // A hand made change of TimeOfDay restarts the cycle from that time.
    const f32 timeSetting = m_timeOfDay.get();
    if (timeSetting != m_lastTimeSetting) {
        m_lastTimeSetting = timeSetting;
        m_cycleHours = 0.0;
    }
    const f32 dayLengthMinutes = m_dayLength.get();
    if (dayLengthMinutes > 0.0F) {
        m_cycleHours =
            std::fmod(m_cycleHours + deltaSeconds * 24.0 / (static_cast<f64>(dayLengthMinutes) * 60.0), 24.0);
    }
    m_state = SkyState{
        .hours = static_cast<f32>(std::fmod(static_cast<f64>(timeSetting) + m_cycleHours, 24.0)),
        .sunSizeDegrees = m_sunSize.get(),
        .skyEnabled = m_enabled.get(),
        .fogEnabled = m_fogEnabled.get(),
        .fogStart = m_fogStart.get(),
        .fogDensity = m_fogDensity.get(),
        .fogHeightFalloff = m_fogFalloff.get(),
        .fogBaseHeight = m_fogBase.get(),
    };
    m_params = computeSkyParams(m_state);
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), &m_params, sizeof(m_params));
    if (m_stats != nullptr) {
        m_stats->set(m_timeStat, static_cast<f64>(m_state.hours));
        m_stats->set(m_elevationStat, static_cast<f64>(glm::degrees(std::asin(m_params.sunDirection.y))));
    }
}

void SkyRenderer::record(rhi::CommandList& commands, rhi::TextureHandle sceneDepth) const {
    commands.bindPipeline(m_pipeline);
    bindParams(commands, 0);
    commands.bindTexture(2, 0, sceneDepth, m_sampler);
    commands.draw(3);
}

void SkyRenderer::bindParams(rhi::CommandList& commands, u32 slot) const {
    commands.bindUniformBuffer(1, slot, m_ring.current(), 0, sizeof(SkyParams));
}

} // namespace vxe::render
