#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/device.hpp>

#include <array>
#include <span>
#include <string_view>

namespace vxe::render {

// A few GPU buffers the CPU writes in turn, one per frame in flight, for data
// that changes every frame (frame constants, debug lines).
//
// Writing a buffer the GPU may still be reading would corrupt a frame, and
// waiting for the GPU every frame would stall the CPU. With kFrames buffers
// used round robin, the buffer about to be written was last used kFrames - 1
// frames ago, which the GPU has normally finished. A fence per buffer proves
// it; if the GPU is that far behind, next() waits (and counts the wait).
//
// Thread safety: main thread only, like the device.
class CpuWriteRing {
public:
    static constexpr usize kFrames = 3;

    CpuWriteRing() = default;
    CpuWriteRing(const CpuWriteRing&) = delete;
    CpuWriteRing& operator=(const CpuWriteRing&) = delete;
    CpuWriteRing(CpuWriteRing&&) = delete;
    CpuWriteRing& operator=(CpuWriteRing&&) = delete;
    ~CpuWriteRing() = default;

    // Creates kFrames persistently mapped buffers of `bytesPerFrame` each.
    // `usage` gets CpuWrite added. Call destroy() before the device goes away.
    [[nodiscard]] Result<void> create(rhi::Device& device, usize bytesPerFrame, rhi::BufferUsage usage,
                                      std::string_view debugName);
    void destroy(rhi::Device& device);

    // Moves to the next buffer and returns its memory for this frame. Call
    // once per frame, before recording commands that read current().
    [[nodiscard]] std::span<std::byte> next(rhi::Device& device);
    // The buffer returned by the latest next().
    [[nodiscard]] rhi::BufferHandle current() const noexcept { return m_slots[m_index].buffer; }
    [[nodiscard]] usize bytesPerFrame() const noexcept { return m_bytesPerFrame; }
    // How often next() had to wait for the GPU. Should stay 0.
    [[nodiscard]] u64 waits() const noexcept { return m_waits; }

private:
    struct Slot {
        rhi::BufferHandle buffer{};
        rhi::FenceHandle fence{};
    };
    std::array<Slot, kFrames> m_slots{};
    usize m_index = 0;
    bool m_started = false;
    usize m_bytesPerFrame = 0;
    u64 m_waits = 0;
};

} // namespace vxe::render
