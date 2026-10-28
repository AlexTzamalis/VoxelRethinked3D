#include "vxe/render/frame_constants.hpp"

#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe::render {

FrameConstants makeFrameConstants(const Camera& camera, const FrameInfo& frame) noexcept {
    const u32 width = std::max(frame.width, 1U);
    const u32 height = std::max(frame.height, 1U);
    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    FrameConstants constants;
    constants.view = camera.view();
    constants.projection = camera.projection(aspect);
    constants.viewProjection = constants.projection * constants.view;
    constants.inverseViewProjection = glm::inverse(constants.viewProjection);
    const CameraPosition position = camera.renderPosition();
    constants.cameraChunk = glm::ivec4(position.chunk.x, position.chunk.y, position.chunk.z, 0);
    constants.cameraFraction = Vec4(position.fraction, 0.0F);
    constants.cameraForward = Vec4(camera.forward(), 0.0F);
    constants.viewport = Vec4(static_cast<f32>(width), static_cast<f32>(height),
                              1.0F / static_cast<f32>(width), 1.0F / static_cast<f32>(height));
    constexpr f64 kTimeWrapSeconds = 3600.0;
    constexpr u64 kFrameIndexMask = (1U << 24U) - 1U;
    constants.time = Vec4(static_cast<f32>(std::fmod(frame.seconds, kTimeWrapSeconds)),
                          static_cast<f32>(frame.deltaSeconds),
                          static_cast<f32>(frame.frameIndex & kFrameIndexMask), 0.0F);
    return constants;
}

Result<void> FrameConstantsBuffer::create(rhi::Device& device) {
    return m_ring.create(device, sizeof(FrameConstants), rhi::BufferUsage::Uniform, "Frame constants");
}

void FrameConstantsBuffer::destroy(rhi::Device& device) {
    m_ring.destroy(device);
}

void FrameConstantsBuffer::update(rhi::Device& device, const FrameConstants& constants) {
    m_current = constants;
    const std::span<std::byte> memory = m_ring.next(device);
    std::memcpy(memory.data(), &constants, sizeof(constants));
}

void FrameConstantsBuffer::bind(rhi::CommandList& commands) const {
    commands.bindUniformBuffer(0, 0, m_ring.current(), 0, sizeof(FrameConstants));
}

} // namespace vxe::render
