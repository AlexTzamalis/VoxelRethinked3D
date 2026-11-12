#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <memory>

namespace vxe::render {

// Where the eye is this frame, from the game, which knows its blocks.
struct WaterView {
    bool underWater = false;
    f32 depth = 0.0F;  // Blocks from the eye up to the surface above it.
    Vec3 color{0.25F}; // The water's own color, linear (its tint times its texture).
};

struct WaterDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA16Float; // The scene's.
};

// How water looks from above and from inside it.
//
// From above: water gets deeper in color with the depth behind its surface,
// clear over a shallow floor and dark blue over the deep, like real water
// that swallows light as it goes. The surfaces of liquids (texture layers
// with voxel::kTextureLookLiquid) read how far the solid scene lies behind
// them from a copy of the depth made before they draw (they write none).
//
// From inside: a fog of the water's color closes in at a few dozen blocks
// and hides the sky, darker the deeper the eye is below the surface and at
// night.
//
// Settings (Graphics.Water.*, live): DepthColor, UnderwaterFog, Clarity.
//
// Thread safety: main thread only.
class WaterRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<WaterRenderer>> create(const WaterDesc& desc);
    ~WaterRenderer();
    WaterRenderer(const WaterRenderer&) = delete;
    WaterRenderer& operator=(const WaterRenderer&) = delete;
    WaterRenderer(WaterRenderer&&) = delete;
    WaterRenderer& operator=(WaterRenderer&&) = delete;

    // The eye's place this frame; stays until set again.
    void setView(const WaterView& view) noexcept { m_view = view; }
    // Uploads this frame's parameters: the sky's light color and how strong
    // daylight is (both dim the water at night). Once per frame.
    void update(const Vec3& skyLight, f32 daylight);
    // Copies the scene depth into an R32F target (cleared), for the surfaces.
    void recordDepthCopy(rhi::CommandList& commands, rhi::TextureHandle sceneDepth) const;
    // Binds what liquid surfaces read in the translucent chunk pass: the
    // parameters (set 1, slot 4) and the depth copy (set 2, slot 8).
    void bindSurfaces(rhi::CommandList& commands, rhi::TextureHandle depthCopy) const;
    // The fog under water over the scene color, from the scene depth.
    void recordFog(rhi::CommandList& commands, rhi::TextureHandle sceneDepth) const;
    [[nodiscard]] bool isFogging() const noexcept { return m_view.underWater && m_fog.get(); }

private:
    explicit WaterRenderer(const WaterDesc& desc);

    rhi::Device& m_device;
    rhi::PipelineHandle m_copyPipeline;
    rhi::PipelineHandle m_fogPipeline;
    rhi::SamplerHandle m_nearest;
    CpuWriteRing m_ring;
    WaterView m_view{};
    config::Setting<bool> m_depthColor;
    config::Setting<bool> m_fog;
    config::Setting<f32> m_clarity;
};

} // namespace vxe::render
