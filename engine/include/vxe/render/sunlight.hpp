#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/clouds.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/render/shadows.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <memory>
#include <span>
#include <vector>

namespace vxe::render {

// The terrain grid of long shadows: 512 cells of 8 blocks, about 4 km across,
// centered on the camera by whoever fills it.
inline constexpr u32 kTerrainShadowCells = 512;
inline constexpr i32 kTerrainShadowCellBlocks = 8;

// The layout must match SunlightParams in include/sunlight.glsl (std140).
struct SunlightParams {
    Vec4 terrain{0.0F}; // xy: grid corner from the camera, z: blocks per cell, w: cells (0 off).
    Vec4 heights{0.0F}; // x: camera world y, y: bias, z: softness, w: strength.
    Vec4 cloud{0.0F};   // x: floor, y: threshold, z: strength (0 off), w: thickness.
    Vec4 cloudAt{0.0F}; // xy: the camera in the cloud map's repeating space.
    Vec4 toLight{0.0F, 1.0F, 0.0F, 0.0F};
};

// Terrain heights for the grid: kTerrainShadowCells squared, row by row along
// x, cell (0, 0) at block column (originX, originZ).
struct TerrainHeightGrid {
    i64 originX = 0;
    i64 originZ = 0;
    std::vector<f32> heights; // voxel::kNoLodHeight where nothing is known.
};

struct SunlightDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    rhi::ShaderFileReader shaders{};
};

// Shadows the shadow map cannot reach (asked by Alex 2026-11-12): mountains
// and far terrain casting over the land behind them, and clouds over the
// ground below them, at every distance the far terrain draws.
//
// Mountains: the world hands in a grid of terrain heights around the camera
// (from the far terrain tiles). A compute pass (render/terrain_shadow.comp)
// finds, for each column, how high the shadow of the land towards the light
// reaches; chunks and far terrain then compare their height with it, one
// texture read per pixel. The pass runs again when the heights change or the
// light has turned a little, not every frame.
//
// Clouds: each pixel follows the line to the light up to the cloud layer and
// reads the cloud map there (CloudRenderer::shadowInfo).
//
// Settings (live): Graphics.Shadows.Terrain and Graphics.Shadows.Clouds.
// Thread safety: main thread only.
class SunlightShadows {
public:
    [[nodiscard]] static Result<std::unique_ptr<SunlightShadows>> create(const SunlightDesc& desc);
    ~SunlightShadows();
    SunlightShadows(const SunlightShadows&) = delete;
    SunlightShadows& operator=(const SunlightShadows&) = delete;
    SunlightShadows(SunlightShadows&&) = delete;
    SunlightShadows& operator=(SunlightShadows&&) = delete;

    // New heights (a world moved its grid or got new far terrain).
    void setTerrainHeights(const TerrainHeightGrid& grid);
    // Forgets the heights (leaving a world).
    void clearTerrain();
    // Where the grid is, for whoever fills it: cell (0, 0) of the last heights.
    [[nodiscard]] bool hasTerrain() const noexcept { return m_hasTerrain; }

    // This frame's light and clouds. Once per frame, after the clouds updated.
    void update(const Camera& camera, const ShadowLight& light, const CloudShadowInfo& clouds);
    // The compute pass, when the heights or the light changed enough.
    void recordCompute(rhi::CommandList& commands);
    // Binds the parameters and textures chunks and far terrain read (set 1
    // slot 5, set 2 slots 6 and 7).
    void bind(rhi::CommandList& commands) const;
    // This frame's parameters (the light shafts read them too).
    [[nodiscard]] const SunlightParams& params() const noexcept { return m_params; }

private:
    explicit SunlightShadows(const SunlightDesc& desc);

    rhi::Device& m_device;
    rhi::PipelineHandle m_compute;
    rhi::TextureHandle m_heights;    // Input: R32F, one texel per cell.
    rhi::TextureHandle m_shadowTops; // Output: R32F.
    rhi::SamplerHandle m_nearest;
    rhi::SamplerHandle m_linear;
    rhi::BufferHandle m_computeParams;
    CpuWriteRing m_ring;
    SunlightParams m_params{};
    CloudShadowInfo m_clouds{};
    i64 m_originX = 0;
    i64 m_originZ = 0;
    f32 m_highest = 0.0F; // Of the heights, so the walk knows when to stop.
    f32 m_lowest = 0.0F;
    bool m_hasTerrain = false;
    bool m_dirty = false;
    Vec3 m_computedLight{0.0F}; // The light the last pass used.
    config::Setting<bool> m_terrainEnabled;
    config::Setting<bool> m_cloudsEnabled;
};

} // namespace vxe::render
