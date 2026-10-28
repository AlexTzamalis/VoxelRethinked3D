#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>

namespace vxe::render {

// Per frame shader data (binding set 0, slot 0, Section 7.3), laid out like
// the std140 block Frame in shaders/include/frame.glsl. Every member is a
// 16 byte multiple, so C++ and std140 agree without padding rules.
struct FrameConstants {
    Mat4 viewProjection{1.0F}; // Camera relative positions to clip space.
    Mat4 view{1.0F};           // Rotation only (camera relative).
    Mat4 projection{1.0F};     // Reversed Z, infinite far plane (ADR-010).
    Mat4 inverseViewProjection{1.0F};
    glm::ivec4 cameraChunk{0}; // xyz: the camera's chunk (Section 8.4).
    Vec4 cameraFraction{0.0F}; // xyz: the camera inside its chunk, in blocks.
    Vec4 cameraForward{0.0F};  // xyz: unit view direction.
    Vec4 viewport{0.0F};       // width, height, 1 / width, 1 / height in pixels.
    // x: seconds since start, wrapped every hour so float keeps millisecond
    // precision for animations; y: frame delta seconds; z: frame index
    // modulo 2^24 (exact in float); w: unused.
    Vec4 time{0.0F};
};
static_assert(sizeof(FrameConstants) == 4 * 64 + 5 * 16);

struct FrameInfo {
    u32 width = 1; // Render target size in pixels.
    u32 height = 1;
    f64 seconds = 0.0;
    f64 deltaSeconds = 0.0;
    u64 frameIndex = 0;
};

[[nodiscard]] FrameConstants makeFrameConstants(const Camera& camera, const FrameInfo& frame) noexcept;

// The uniform buffer behind FrameConstants: written once per frame through a
// CpuWriteRing and bound at set 0, slot 0 for every pass.
//
// Thread safety: main thread only.
class FrameConstantsBuffer {
public:
    [[nodiscard]] Result<void> create(rhi::Device& device);
    void destroy(rhi::Device& device);
    // Writes this frame's constants. Once per frame, before recording.
    void update(rhi::Device& device, const FrameConstants& constants);
    // Binds the latest constants (set 0, slot 0).
    void bind(rhi::CommandList& commands) const;
    [[nodiscard]] const FrameConstants& current() const noexcept { return m_current; }

private:
    CpuWriteRing m_ring;
    FrameConstants m_current{};
};

} // namespace vxe::render
