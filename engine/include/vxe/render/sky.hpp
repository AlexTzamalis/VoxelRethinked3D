#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <memory>

namespace vxe::render {

// The sky pass's uniform block (set 1, slot 0), laid out like SkyParams in
// shaders/render/sky.frag (std140, all members 16 bytes).
struct SkyParams {
    Vec4 sunDirection{0.0F, 1.0F, 0.0F, 1.0F}; // xyz: towards the sun; w: cosine of the disk radius.
    Vec4 zenithColor{0.0F};
    Vec4 horizonColor{0.0F};
    Vec4 sunColor{0.0F}; // rgb: disk and glow color; a: glow strength.
    Vec4 fog{0.0F};      // start, density, height falloff, base height.
    glm::ivec4 flags{0}; // x: sky on, y: fog on.
};
static_assert(sizeof(SkyParams) == 6 * 16);

// What the sky looks like at one moment, before it becomes shader data.
struct SkyState {
    f32 hours = 10.0F;         // Time of day, 0 to 24. Sunrise at 6, noon at 12, sunset at 18.
    f32 sunSizeDegrees = 1.2F; // Angular radius of the sun disk.
    bool skyEnabled = true;
    bool fogEnabled = true;
    f32 fogStart = 32.0F;
    f32 fogDensity = 0.0015F;
    f32 fogHeightFalloff = 0.02F;
    f32 fogBaseHeight = 64.0F;
};

// The shader data for a sky state: the sun's direction from the time of day
// and the colors from the sun's height (blue day, orange low sun, dark night).
// Pure, so tests can check it without a GPU.
[[nodiscard]] SkyParams computeSkyParams(const SkyState& state) noexcept;

struct SkyDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA8Unorm; // The scene color the pass draws into.
};

// The analytical sky, sun and height fog (Section 8.1 pass 6, E8.3), drawn
// as one fullscreen pass over the scene color while sampling the scene depth:
// the sky where nothing was drawn, fog blended over everything else.
//
// Settings, all live (Graphics.Sky.*): Enabled, TimeOfDay, DayLengthMinutes
// (0 stops the clock), SunSize, FogEnabled, FogStart, FogDensity,
// FogHeightFalloff, FogBaseHeight. Changing TimeOfDay by hand restarts the
// day cycle from that time. Stats: Render.Sky.TimeOfDay and
// Render.Sky.SunElevation.
//
// Thread safety: main thread only.
class SkyRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<SkyRenderer>> create(const SkyDesc& desc);
    ~SkyRenderer();
    SkyRenderer(const SkyRenderer&) = delete;
    SkyRenderer& operator=(const SkyRenderer&) = delete;
    SkyRenderer(SkyRenderer&&) = delete;
    SkyRenderer& operator=(SkyRenderer&&) = delete;

    // Advances the day cycle, reads the settings and uploads this frame's
    // parameters. Once per frame, before recording.
    void update(f64 deltaSeconds);
    // Draws the sky and fog. Record in a raster pass with only the scene
    // color attached (load), after binding the frame constants. `sceneDepth`
    // is the depth the scene was drawn with, sampled, not attached.
    void record(rhi::CommandList& commands, rhi::TextureHandle sceneDepth) const;

    [[nodiscard]] const SkyState& state() const noexcept { return m_state; }
    [[nodiscard]] const SkyParams& params() const noexcept { return m_params; }
    // Binds this frame's sky parameters at set 1, `slot`, for shaders that
    // include include/sky.glsl with VXE_SKY_SLOT (translucent chunks fog
    // themselves with it). Valid after update().
    void bindParams(rhi::CommandList& commands, u32 slot) const;

private:
    explicit SkyRenderer(const SkyDesc& desc);

    rhi::Device& m_device;
    stats::StatRegistry* m_stats;
    rhi::PipelineHandle m_pipeline;
    rhi::SamplerHandle m_sampler;
    CpuWriteRing m_ring;
    SkyState m_state;
    SkyParams m_params;
    f32 m_lastTimeSetting = -1.0F;
    f64 m_cycleHours = 0.0; // Hours the day cycle advanced since TimeOfDay was last set.

    config::Setting<bool> m_enabled;
    config::Setting<f32> m_timeOfDay;
    config::Setting<f32> m_dayLength;
    config::Setting<f32> m_sunSize;
    config::Setting<bool> m_fogEnabled;
    config::Setting<f32> m_fogStart;
    config::Setting<f32> m_fogDensity;
    config::Setting<f32> m_fogFalloff;
    config::Setting<f32> m_fogBase;
    stats::StatId m_timeStat;
    stats::StatId m_elevationStat;
};

} // namespace vxe::render
