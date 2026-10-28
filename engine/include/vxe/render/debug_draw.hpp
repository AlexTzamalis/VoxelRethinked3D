#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <memory>
#include <span>
#include <vector>

namespace vxe::render {

// Packs a color for debug drawing. Red is the low byte, matching GLSL's
// unpackUnorm4x8.
[[nodiscard]] constexpr u32 debugColor(u8 red, u8 green, u8 blue, u8 alpha = 255) noexcept {
    return static_cast<u32>(red) | (static_cast<u32>(green) << 8U) | (static_cast<u32>(blue) << 16U) |
           (static_cast<u32>(alpha) << 24U);
}

// One end of a debug line, as the line shader reads it (std430: vec3 then
// uint is 16 bytes).
struct LineVertex {
    Vec3 position{0.0F}; // Relative to the camera, in blocks.
    u32 color = 0;
};
static_assert(sizeof(LineVertex) == 16);

struct DebugDrawDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    input::InputSystem* input = nullptr;  // Optional: the F3+G chunk border key.
    // The always active context for F3+G, usually the overlay's Debug context.
    input::InputContextId toggleContext{};
    rhi::ShaderFileReader shaders{};
    // The attachments of the pass that records the lines.
    rhi::Format colorFormat = rhi::Format::RGBA8Unorm;
    rhi::Format depthFormat = rhi::Format::D32Float;
    u32 maxLines = 65536; // More lines in one frame are dropped and counted.
};

// Immediate mode debug lines in world space: boxes, grids, chunk borders
// (Section 18.3), collected during the frame and drawn in one draw call.
//
// Positions are given in world coordinates as double and made camera
// relative on the CPU, in double, before they become floats, so lines do not
// shake at 10 million blocks from the origin (ADR-011). The vertices go
// through a CpuWriteRing and the vertex shader pulls them from a storage
// buffer; drawing needs the frame constants bound (FrameConstantsBuffer).
//
// Settings: Debug.Draw.ChunkBorders (F3+G), Debug.Draw.ChunkBorderRadius and
// Debug.Draw.DepthTest (off draws lines through everything). Stats:
// Render.DebugDraw.Lines and Render.DebugDraw.Dropped. Recording a frame
// does not allocate.
//
// Thread safety: main thread only.
class DebugDraw {
public:
    [[nodiscard]] static Result<std::unique_ptr<DebugDraw>> create(const DebugDrawDesc& desc);
    ~DebugDraw();
    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;
    DebugDraw(DebugDraw&&) = delete;
    DebugDraw& operator=(DebugDraw&&) = delete;

    // Reads the F3+G key. Call after InputSystem::update.
    void handleInput();
    // Starts a frame: forgets the last frame's lines and makes `camera` the
    // origin of this frame's positions. Adds the chunk borders when they are on.
    void beginFrame(const Camera& camera);

    void line(const DVec3& from, const DVec3& to, u32 color);
    void box(const DVec3& min, const DVec3& max, u32 color);
    void chunkBox(const ChunkPos& chunk, u32 color);
    // Lines in the horizontal plane at height `y`: every `spacing` blocks in
    // a square of 2 * halfCount lines around `center` snapped to the
    // spacing, and every majorEvery-th line in `majorColor`.
    void grid(const DVec3& center, f64 y, i32 halfCount, f64 spacing, u32 color, u32 majorColor,
              i32 majorEvery);

    // Uploads and draws this frame's lines. Record inside a raster pass with
    // the formats from the desc, after binding the frame constants.
    void record(rhi::CommandList& commands);

    [[nodiscard]] std::span<const LineVertex> vertices() const noexcept { return m_vertices; }
    [[nodiscard]] u32 lineCount() const noexcept { return static_cast<u32>(m_vertices.size() / 2); }
    [[nodiscard]] u64 droppedLines() const noexcept { return m_dropped; }

private:
    explicit DebugDraw(const DebugDrawDesc& desc);
    void addChunkBorders();

    rhi::Device& m_device;
    stats::StatRegistry* m_stats;
    input::InputSystem* m_input;
    u32 m_maxLines;
    rhi::PipelineHandle m_depthTested;
    rhi::PipelineHandle m_onTop;
    CpuWriteRing m_ring;
    std::vector<LineVertex> m_vertices; // Capacity reserved once; never grows.
    DVec3 m_origin{0.0};
    ChunkPos m_cameraChunk{};
    u64 m_dropped = 0;
    u64 m_droppedThisFrame = 0;
    config::Setting<bool> m_chunkBorders;
    config::Setting<i32> m_chunkBorderRadius;
    config::Setting<bool> m_depthTest;
    input::ActionId m_toggleChunkBorders;
    stats::StatId m_linesStat;
    stats::StatId m_droppedStat;
};

} // namespace vxe::render
