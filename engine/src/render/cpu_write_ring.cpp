#include "vxe/render/cpu_write_ring.hpp"

#include <format>
#include <thread>

namespace vxe::render {

Result<void> CpuWriteRing::create(rhi::Device& device, usize bytesPerFrame, rhi::BufferUsage usage,
                                  std::string_view debugName) {
    m_bytesPerFrame = bytesPerFrame;
    for (usize i = 0; i < kFrames; ++i) {
        const std::string name = std::format("{} {}", debugName, i);
        auto buffer = device.createBuffer(
            {.sizeBytes = bytesPerFrame, .usage = usage | rhi::BufferUsage::CpuWrite, .debugName = name});
        if (!buffer) {
            destroy(device);
            return buffer.error();
        }
        m_slots[i].buffer = buffer.value();
    }
    return {};
}

void CpuWriteRing::destroy(rhi::Device& device) {
    for (Slot& slot : m_slots) {
        device.destroy(slot.buffer);
        device.destroy(slot.fence);
        slot = {};
    }
    m_started = false;
}

std::span<std::byte> CpuWriteRing::next(rhi::Device& device) {
    // The fence for the buffer used last frame goes in now: that frame has
    // been submitted, so the fence signals once the GPU finished it.
    if (m_started) {
        Slot& previous = m_slots[m_index];
        device.destroy(previous.fence);
        previous.fence = device.insertFence();
        m_index = (m_index + 1) % kFrames;
    }
    m_started = true;
    Slot& slot = m_slots[m_index];
    if (slot.fence.isValid()) {
        if (!device.isFenceSignaled(slot.fence)) {
            ++m_waits;
            while (!device.isFenceSignaled(slot.fence)) {
                std::this_thread::yield();
            }
        }
        device.destroy(slot.fence);
        slot.fence = {};
    }
    return device.mappedRange(slot.buffer);
}

} // namespace vxe::render
