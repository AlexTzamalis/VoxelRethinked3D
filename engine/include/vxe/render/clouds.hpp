#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/render/texture_pack.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace vxe::render {

class SkyRenderer;

inline constexpr i32 kCloudMapSize = 512;  // Cells along each side; also in clouds.frag.
inline constexpr i32 kCloudCellBlocks = 8; // Blocks across a cell; also in clouds.frag.
inline constexpr u32 kCloudMapLevels = 5;  // Mip levels, the last of 32 x 32 cells.

// Where clouds stand on the grid, as mip levels of 8 bit densities, finest first.
// Each value is the share of all cells that are below it, so a threshold of 0.6
// leaves exactly 40% of the sky covered, whatever the noise did. Repeats at its
// edges without a seam. A pure function of the seed, so a world keeps its clouds.
[[nodiscard]] std::array<std::vector<u8>, kCloudMapLevels> buildCloudMap(u64 seed);

// Noise of a pack that wears the edges of clouds away: smooth in red, finer in green.
inline constexpr std::string_view kCloudNoiseImage = "environment/noise.png";

// Graphics.Clouds.Quality: steps along each ray, the main cost of clouds.
enum class CloudQuality : u8 {
    Low,
    Medium,
    High
};

struct CloudDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA8Unorm; // The scene color the pass draws into.
};

// Volumetric clouds (G9.5): a volume marched at half resolution into its own
// layer, which then goes over the scene. See clouds.frag for how they are drawn.
//
// Settings, all live (Graphics.Clouds.*): Enabled, Quality, Height, Thickness,
// CoverScale, Softness, Blockiness and Speed.
//
// Thread safety: main thread only.
class CloudRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<CloudRenderer>> create(const CloudDesc& desc);
    ~CloudRenderer();
    CloudRenderer(const CloudRenderer&) = delete;
    CloudRenderer& operator=(const CloudRenderer&) = delete;
    CloudRenderer(CloudRenderer&&) = delete;
    CloudRenderer& operator=(CloudRenderer&&) = delete;

    // Makes the clouds of a world. Takes a few milliseconds, for world entry.
    void setSeed(u64 seed);
    // Moves the wind and uploads this frame's parameters. Once per frame.
    // `cover` is the share of the sky under clouds (Atmosphere::cloudCover), and
    // `timeSpeed` scales the wind with the day (SkyRenderer::timeSpeed).
    void update(f64 deltaSeconds, f32 cover, f32 timeSpeed);
    // The noise of kCloudNoiseImage, or nothing for plain edges.
    void setNoise(const PackImage* image);
    // Draws the clouds into their layer: a half size target cleared to nothing,
    // with the frame constants bound.
    void recordLayer(rhi::CommandList& commands, rhi::TextureHandle sceneDepth, const SkyRenderer& sky) const;
    // Lays that layer over the scene color (load).
    void recordBlend(rhi::CommandList& commands, rhi::TextureHandle layer) const;

private:
    explicit CloudRenderer(const CloudDesc& desc);

    rhi::Device& m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::PipelineHandle m_blendPipeline;
    rhi::SamplerHandle m_sampler;      // Nearest, for the depth.
    rhi::SamplerHandle m_mapSampler;   // Smooth and repeating.
    rhi::SamplerHandle m_layerSampler; // Smooth, for the upscale.
    rhi::TextureHandle m_map;
    rhi::TextureHandle m_noise; // 1 x 1 until a pack gives one.
    bool m_noiseLoaded = false;
    CpuWriteRing m_ring;
    f64 m_windX = 0.0; // Blocks, wrapped at the size of the map.
    f64 m_windZ = 0.0;

    config::Setting<bool> m_enabled;
    config::Setting<f32> m_height;
    config::Setting<f32> m_thickness;
    config::Setting<f32> m_coverScale;
    f32 m_cover = 0.0F; // This frame's share of sky.
    config::Setting<f32> m_softness;
    config::Setting<f32> m_blockiness;
    config::Setting<CloudQuality> m_quality;
    config::Setting<f32> m_speed;
};

} // namespace vxe::render
