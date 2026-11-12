#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <memory>

namespace vxe::render {

// Where the sun stands on the screen and how strongly it shines through.
struct GodRayView {
    Vec2 sunUv{0.5F}; // 0 to 1 across the screen; may lie a little outside it.
    f32 strength = 0.0F;
};

// The sun's place on screen for a camera, and the strength of its rays: full
// while it is in view and above the horizon, fading as it leaves the screen,
// sets or falls behind the camera.
[[nodiscard]] GodRayView godRayView(const Camera& camera, f32 aspect, const Vec3& toSun) noexcept;

struct GodRayDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA16Float; // Of the scene and of the ray layer.
};

// God rays (sun shafts), at half resolution: each pixel marches towards the
// sun on the screen and adds up how much open sky it passes, so gaps between
// leaves, mountain edges and clouds light up as shafts; and it follows its view
// ray through the air, where the shadow map and the long shadows say which bits
// the sun reaches, so lit air shows as beams from every side. The layer is
// then added over the scene in the light's color.
//
// Settings: Graphics.Sky.GodRays, GodRayStrength and LightInAir, live.
// Thread safety: main thread only.
class GodRayRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<GodRayRenderer>> create(const GodRayDesc& desc);
    ~GodRayRenderer();
    GodRayRenderer(const GodRayRenderer&) = delete;
    GodRayRenderer& operator=(const GodRayRenderer&) = delete;
    GodRayRenderer(GodRayRenderer&&) = delete;
    GodRayRenderer& operator=(GodRayRenderer&&) = delete;

    // Once per frame: the camera, the screen's aspect, the sun's direction,
    // the color of the light and how strongly it shines (ShadowLight::strength;
    // the light in the air follows the sun or the moon, whichever casts).
    void update(const Camera& camera, f32 aspect, const Vec3& toSun, const Vec3& sunColor, f32 lightStrength);
    // The screen shafts towards the sun show (the sun in view).
    [[nodiscard]] bool isShining() const noexcept;
    // Anything to draw: the screen shafts or the light in the air.
    [[nodiscard]] bool isDrawing() const noexcept { return isShining() || m_air > 0.001F; }
    // The ray layer, from the scene depth and the cloud layer (its alpha hides
    // the sun). The light in the air also needs the frame constants, the
    // shadows (ChunkRenderer::bindShadows) and SunlightShadows::bind bound.
    void recordLayer(rhi::CommandList& commands, rhi::TextureHandle sceneDepth,
                     rhi::TextureHandle clouds) const;
    // Adds the layer over the scene.
    void recordBlend(rhi::CommandList& commands, rhi::TextureHandle layer) const;

private:
    explicit GodRayRenderer(const GodRayDesc& desc);

    rhi::Device& m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::PipelineHandle m_blendPipeline;
    rhi::SamplerHandle m_nearest;
    rhi::SamplerHandle m_linear;
    CpuWriteRing m_ring;
    GodRayView m_view{};
    config::Setting<bool> m_enabled;
    config::Setting<f32> m_strength;
    config::Setting<f32> m_airStrength;
    f32 m_air = 0.0F; // This frame's strength of the light in the air.
};

} // namespace vxe::render
