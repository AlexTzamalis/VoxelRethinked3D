#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/render/texture_pack.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace vxe::render {

// Columns of rain or snow stand on this many blocks around the camera in each
// direction; the grid is kPrecipitationSide blocks wide. Also in precipitation.vert.
inline constexpr i32 kPrecipitationReach = 16;
inline constexpr i32 kPrecipitationSide = (kPrecipitationReach * 2) + 1;

// The streak images of a pack, in the order of PrecipitationKind after None: four
// streaks side by side, each one block wide and as tall as the image allows.
inline constexpr std::array<std::string_view, 2> kPrecipitationImages = {"environment/rain.png",
                                                                         "environment/snow.png"};
inline constexpr u32 kPrecipitationImageWidth = 64; // Others are scaled to this.
inline constexpr u32 kPrecipitationImageHeight = 256;

enum class PrecipitationKind : u8 {
    None,
    Rain,
    Snow
};

struct PrecipitationDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA8Unorm;
    rhi::Format depthFormat = rhi::Format::D32Float;
};

// Falling rain and snow around the camera, as Minecraft draws them: one upright
// quad per block column, turned towards the viewer, with a streak image that
// scrolls down it. Each column ends on the highest block the world says it
// has, so nothing falls under a roof, a tree or into a cave. How much of the
// grid has streaks follows the amount, so a shower thickens into a downpour.
//
// The game tells it the weather (setWeather) and the tops of the columns
// (setColumns); the renderer only draws.
//
// Settings: Graphics.Sky.Precipitation, live.
//
// Thread safety: main thread only.
class PrecipitationRenderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<PrecipitationRenderer>> create(const PrecipitationDesc& desc);
    ~PrecipitationRenderer();
    PrecipitationRenderer(const PrecipitationRenderer&) = delete;
    PrecipitationRenderer& operator=(const PrecipitationRenderer&) = delete;
    PrecipitationRenderer(PrecipitationRenderer&&) = delete;
    PrecipitationRenderer& operator=(PrecipitationRenderer&&) = delete;

    // The images of kPrecipitationImages, in that order. A missing one draws nothing.
    void setImages(std::span<const PackImage* const> images);
    // What falls, and how much of it from 0 (none) to 1 (a downpour).
    void setWeather(PrecipitationKind kind, f32 amount) noexcept;
    // The world height of the highest block of each column of the grid, row by
    // row from (cornerX, cornerZ), kPrecipitationSide squared values. A column
    // nothing was loaded for can say kOpenColumn, and streaks reach the bottom.
    void setColumns(i64 cornerX, i64 cornerZ, std::span<const i32> tops);
    static constexpr i32 kOpenColumn = -0x40000000;

    // Uploads this frame's grid and parameters. Once per frame.
    void update(const Camera& camera, f64 seconds, const Vec3& skyLight);
    // Draws the streaks, in a pass with the scene color and depth attached
    // (both load) and the frame constants bound.
    void record(rhi::CommandList& commands) const;

    [[nodiscard]] bool isFalling() const noexcept;

private:
    explicit PrecipitationRenderer(const PrecipitationDesc& desc);

    rhi::Device& m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::SamplerHandle m_sampler;
    rhi::TextureHandle m_streaks;     // Rain and snow, one layer each.
    rhi::TextureHandle m_topsTexture; // Column tops, relative to the camera's chunk.
    std::array<bool, 2> m_loaded{};
    CpuWriteRing m_ring;
    PrecipitationKind m_kind = PrecipitationKind::None;
    f32 m_amount = 0.0F;
    i64 m_cornerX = 0;
    i64 m_cornerZ = 0;
    std::vector<i32> m_tops;
    std::vector<f32> m_relativeTops; // Reused every frame.

    config::Setting<bool> m_enabled;
};

} // namespace vxe::render
