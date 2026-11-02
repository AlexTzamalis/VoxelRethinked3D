#include "vxe/render/sky.hpp"

#include <vxe/render/post_process.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vxe::render {
namespace {

// The sun's path is tilted this far from straight overhead, so noon light
// comes from the side and terrain gets shape.
constexpr f32 kSunPathTilt = 0.5F; // Radians, about 29 degrees.
constexpr f32 kNormalDayMinutes = 20.0F;

constexpr std::array<config::EnumSettingDesc<WeatherChoice>::Option, 6> kWeatherOptions = {
    {{WeatherChoice::Auto, "Auto", "Auto"},
     {WeatherChoice::Clear, "Clear", "Clear"},
     {WeatherChoice::Fair, "Fair", "Fair"},
     {WeatherChoice::Cloudy, "Cloudy", "Cloudy"},
     {WeatherChoice::Overcast, "Overcast", "Overcast"},
     {WeatherChoice::Rain, "Rain", "Rain"}}};

const Vec3 kMoonTint(0.92F, 0.95F, 1.05F);
const Vec3 kMoonLight(0.24F, 0.3F, 0.46F); // On terrain, under a full moon.
// How bright each phase is, full first. A new moon still leaves starlight.
constexpr std::array<f32, kMoonPhases> kMoonPhaseLight = {1.0F,  0.86F, 0.68F, 0.5F,
                                                          0.36F, 0.5F,  0.68F, 0.86F};

} // namespace

SkyParams computeSkyParams(const SkyState& state) noexcept {
    // Colors picked as they look on screen (sRGB). decodeSrgb turns them into
    // the linear values the HDR scene and the tonemapper (E8.4) work with.
    const Vec3 kDayZenith = decodeSrgb(Vec3(0.20F, 0.42F, 0.82F));
    const Vec3 kDayHorizon = decodeSrgb(Vec3(0.66F, 0.80F, 0.95F));
    const Vec3 kDuskZenith = decodeSrgb(Vec3(0.24F, 0.27F, 0.50F));
    const Vec3 kDuskHorizon = decodeSrgb(Vec3(0.96F, 0.56F, 0.32F));
    const Vec3 kNightZenith = decodeSrgb(Vec3(0.02F, 0.03F, 0.075F));
    const Vec3 kNightHorizon = decodeSrgb(Vec3(0.035F, 0.045F, 0.085F));
    const Vec3 kLowSun = decodeSrgb(Vec3(1.0F, 0.55F, 0.25F));
    const Vec3 kHighSun = decodeSrgb(Vec3(1.0F, 0.95F, 0.85F));

    // Minecraft's path: 0 is noon, and the eased part keeps the sun high for
    // longer than low. It clears the horizon about 5:00 and sets about 19:00,
    // rising in the east (+X).
    const f32 fromNoon = glm::fract((state.hours / 24.0F) - 0.5F);
    const f32 eased = 0.5F - (std::cos(fromNoon * glm::pi<f32>()) * 0.5F);
    const f32 angle = ((fromNoon * 2.0F) + eased) / 3.0F * glm::two_pi<f32>();
    const Vec3 sun = glm::normalize(Vec3(-std::sin(angle), std::cos(angle) * std::cos(kSunPathTilt),
                                         -std::cos(angle) * std::sin(kSunPathTilt)));
    const f32 elevation = sun.y;
    const f32 day = glm::smoothstep(-0.1F, 0.25F, elevation);
    // Dusk colors peak while the sun is near the horizon, and fade out once
    // it is well below it.
    const f32 dusk =
        (1.0F - glm::smoothstep(0.0F, 0.35F, std::abs(elevation))) * glm::smoothstep(-0.2F, 0.0F, elevation);

    // Grey days: color drains out of the sky and the sun loses its strength.
    const f32 grey = glm::clamp(state.overcast, 0.0F, 1.0F);
    const auto drained = [grey](const Vec3& color) {
        const f32 level = glm::dot(color, Vec3(0.3F, 0.59F, 0.11F));
        return glm::mix(color, Vec3(level) * 0.82F, grey);
    };

    SkyParams params;
    params.sunDirection = Vec4(sun, std::cos(glm::radians(state.sunSizeDegrees)));
    params.zenithColor =
        Vec4(drained(glm::mix(glm::mix(kNightZenith, kDayZenith, day), kDuskZenith, dusk * 0.5F)), 1.0F);
    params.horizonColor =
        Vec4(drained(glm::mix(glm::mix(kNightHorizon, kDayHorizon, day), kDuskHorizon, dusk)), 1.0F);
    const f32 visible = glm::smoothstep(-0.05F, 0.02F, elevation);
    params.sunColor = Vec4(glm::mix(kLowSun, kHighSun, glm::smoothstep(0.0F, 0.4F, elevation)) * visible *
                               (1.0F - (grey * 0.85F)),
                           0.35F + 0.65F * dusk);
    params.fog = Vec4(state.fogStart, state.fogDensity, state.fogHeightFalloff, state.fogBaseHeight);
    // The night sky turns with the sun, once a day around the same axis.
    const Vec3 axis(0.0F, std::sin(kSunPathTilt), std::cos(kSunPathTilt));
    params.stars = Vec4(axis, state.hours / 24.0F * glm::two_pi<f32>());
    // Stars come out as the sky darkens. Clear air and a dark moon show more of
    // them, and only clear nights show the galaxy in full.
    const f32 dark = 1.0F - glm::smoothstep(-0.2F, 0.02F, elevation);
    const f32 moonGlare = kMoonPhaseLight.at(state.moonPhase % kMoonPhases) * 0.45F;
    const f32 clear = glm::clamp(state.clearness, 0.0F, 1.0F);
    params.night =
        Vec4(dark * (0.35F + (0.65F * clear)),
             state.galaxyLoaded ? dark * (0.12F + (0.88F * clear * clear)) * (1.0F - moonGlare) : 0.0F, 0.0F,
             0.0F);

    const u32 phase = state.moonPhase % kMoonPhases;
    params.flags = glm::ivec4(state.skyEnabled ? 1 : 0, state.fogEnabled ? 1 : 0, state.bodiesLoaded ? 1 : 0,
                              static_cast<i32>(phase) + 1);

    // The moon stands opposite the sun and shows less the brighter the sky.
    params.sunDirection.w = std::cos(glm::radians(state.sunSizeDegrees));
    params.moonDirection = Vec4(-sun, std::tan(glm::radians(state.sunSizeDegrees)));
    params.moonColor = Vec4(kMoonTint, glm::smoothstep(-0.05F, 0.05F, -elevation) * (1.0F - (day * 0.9F)));

    // What terrain is lit by: white sun, warm near the horizon, and at night a
    // blue moon whose strength follows its phase.
    const f32 low =
        (1.0F - glm::smoothstep(0.05F, 0.45F, elevation)) * glm::smoothstep(-0.12F, 0.02F, elevation);
    const Vec3 sunLight = glm::mix(Vec3(1.0F), Vec3(1.0F, 0.74F, 0.52F), low);
    const Vec3 moonLight = kMoonLight * kMoonPhaseLight.at(phase);
    const Vec3 greyLight = drained(glm::mix(moonLight, sunLight, day)) * (1.0F - (grey * 0.3F));
    params.skyLight = Vec4(state.skyEnabled ? greyLight : Vec3(1.0F), 1.0F);
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
    m_dayLength = number("Graphics.Sky.DayMinutes", "Day length",
                         "Real minutes for a full day, 20 as in Minecraft. 0 stops the sun.", 20.0F, 0.0F,
                         240.0F, 1.0F);
    m_sunSize = number("Graphics.Sky.BodySize", "Sun and moon size",
                       "Half the width of the sun and moon in degrees.", 7.0F, 1.0F, 20.0F, 0.5F);
    config::EnumSettingDesc<WeatherChoice> weather;
    weather.id = "Graphics.Sky.Weather";
    weather.displayName = "Weather";
    weather.description = "Auto follows the world's own weather. The others hold one kind.";
    weather.defaultValue = WeatherChoice::Auto;
    weather.options = kWeatherOptions;
    m_weather = settings.registerEnum(weather);
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
    auto bodySampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                             .magFilter = rhi::Filter::Nearest,
                                             .mipmapMode = rhi::MipmapMode::None,
                                             .addressU = rhi::AddressMode::ClampToEdge,
                                             .addressV = rhi::AddressMode::ClampToEdge,
                                             .debugName = "Sky body sampler"});
    if (!bodySampler) {
        return bodySampler.error().withContext("Sky");
    }
    sky->m_bodySampler = bodySampler.value();
    auto bodies = device.createTexture({.type = rhi::TextureType::Texture2DArray,
                                        .format = rhi::Format::RGBA8Srgb,
                                        .width = kSkyBodySize,
                                        .height = kSkyBodySize,
                                        .layers = kSkyBodyImages.size(),
                                        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                        .debugName = "Sun and moon"});
    if (!bodies) {
        return bodies.error().withContext("Sky");
    }
    sky->m_bodies = bodies.value();
    auto galaxySampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                               .magFilter = rhi::Filter::Linear,
                                               .mipmapMode = rhi::MipmapMode::None,
                                               .addressU = rhi::AddressMode::Repeat,
                                               .addressV = rhi::AddressMode::ClampToEdge,
                                               .debugName = "Galaxy sampler"});
    if (!galaxySampler) {
        return galaxySampler.error().withContext("Sky");
    }
    sky->m_galaxySampler = galaxySampler.value();
    auto galaxy = device.createTexture({.format = rhi::Format::RGBA8Srgb,
                                        .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                        .debugName = "Galaxy"});
    if (!galaxy) {
        return galaxy.error().withContext("Sky");
    }
    sky->m_galaxy = galaxy.value();
    VXE_TRY(sky->m_ring.create(device, sizeof(SkyParams), rhi::BufferUsage::Uniform, "Sky parameters"));
    return sky;
}

SkyRenderer::~SkyRenderer() {
    m_device.destroy(m_pipeline);
    m_device.destroy(m_sampler);
    m_device.destroy(m_bodySampler);
    m_device.destroy(m_bodies);
    m_device.destroy(m_galaxySampler);
    m_device.destroy(m_galaxy);
    m_ring.destroy(m_device);
}

void SkyRenderer::update(f64 deltaSeconds) {
    // A hand made change of TimeOfDay moves the clock to that hour of this day.
    const f32 timeSetting = m_timeOfDay.get();
    if (timeSetting != m_lastTimeSetting) {
        m_lastTimeSetting = timeSetting;
        m_clockHours = (std::floor(m_clockHours / 24.0) * 24.0) + static_cast<f64>(timeSetting);
    }
    const f32 dayLengthMinutes = m_dayLength.get();
    if (dayLengthMinutes > 0.0F) {
        m_clockHours += deltaSeconds * 24.0 / (static_cast<f64>(dayLengthMinutes) * 60.0);
    }
    m_atmosphere = computeAtmosphere(m_weatherSeed, m_clockHours, m_weather.get());
    m_state = SkyState{
        .hours = static_cast<f32>(std::fmod(m_clockHours, 24.0)),
        .sunSizeDegrees = m_sunSize.get(),
        .moonPhase = static_cast<u32>(std::fmod(std::floor(m_clockHours / 24.0), kMoonPhases)),
        .bodiesLoaded = m_bodiesLoaded,
        .galaxyLoaded = m_galaxyLoaded,
        .clearness = m_atmosphere.clearness,
        .overcast = m_atmosphere.overcast,
        .skyEnabled = m_enabled.get(),
        .fogEnabled = m_fogEnabled.get(),
        .fogStart = m_fogStart.get(),
        .fogDensity = m_fogDensity.get() * m_atmosphere.fogScale,
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

f32 SkyRenderer::timeSpeed() const noexcept {
    const f32 minutes = m_dayLength.get();
    return minutes > 0.0F ? kNormalDayMinutes / minutes : 1.0F;
}

void SkyRenderer::setClockHours(f64 hours) noexcept {
    m_clockHours = std::max(hours, 0.0);
    m_lastTimeSetting = m_timeOfDay.get(); // A saved clock is not a hand made change.
}

void SkyRenderer::setBodies(std::span<const PackImage* const> images) {
    std::vector<u32> scaled(usize{kSkyBodySize} * kSkyBodySize);
    bool any = false;
    for (u32 layer = 0; layer < images.size() && layer < kSkyBodyImages.size(); ++layer) {
        const PackImage* image = images[layer];
        if (image == nullptr || image->width == 0 || image->height == 0) {
            continue;
        }
        for (u32 y = 0; y < kSkyBodySize; ++y) {
            for (u32 x = 0; x < kSkyBodySize; ++x) {
                const u32 fromX = (x * image->width) / kSkyBodySize;
                const u32 fromY = (y * image->height) / kSkyBodySize;
                scaled[(usize{y} * kSkyBodySize) + x] = image->pixels[(usize{fromY} * image->width) + fromX];
            }
        }
        static_cast<void>(m_device.uploadTexture(
            m_bodies, {.layer = layer, .data = std::as_bytes(std::span<const u32>(scaled))}));
        any = true;
    }
    m_bodiesLoaded = any;
}

void SkyRenderer::setGalaxy(const PackImage* image) {
    if (image == nullptr || image->width == 0 || image->height == 0) {
        m_galaxyLoaded = false;
        return;
    }
    auto texture =
        m_device.createTexture({.format = rhi::Format::RGBA8Srgb,
                                .width = image->width,
                                .height = image->height,
                                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                .debugName = "Galaxy"});
    if (!texture) {
        m_galaxyLoaded = false;
        return;
    }
    m_device.destroy(m_galaxy);
    m_galaxy = texture.value();
    static_cast<void>(m_device.uploadTexture(m_galaxy, {.data = std::as_bytes(std::span(image->pixels))}));
    m_galaxyLoaded = true;
}

void SkyRenderer::record(rhi::CommandList& commands, rhi::TextureHandle sceneDepth) const {
    commands.bindPipeline(m_pipeline);
    bindParams(commands, 0);
    commands.bindTexture(2, 0, sceneDepth, m_sampler);
    commands.bindTexture(2, 1, m_bodies, m_bodySampler);
    commands.bindTexture(2, 2, m_galaxy, m_galaxySampler);
    commands.draw(3);
}

void SkyRenderer::bindParams(rhi::CommandList& commands, u32 slot) const {
    commands.bindUniformBuffer(1, slot, m_ring.current(), 0, sizeof(SkyParams));
}

} // namespace vxe::render
