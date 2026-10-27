#include "vxe/rhi/gpu_timer.hpp"

namespace vxe::rhi {

Result<void> GpuFrameTimer::create(Device& device) {
    for (Slot& slot : m_slots) {
        auto begin = device.createTimestampQuery();
        if (!begin) {
            destroy(device);
            return begin.error();
        }
        slot.begin = begin.value();
        auto end = device.createTimestampQuery();
        if (!end) {
            destroy(device);
            return end.error();
        }
        slot.end = end.value();
    }
    return {};
}

void GpuFrameTimer::destroy(Device& device) {
    for (Slot& slot : m_slots) {
        device.destroy(slot.begin);
        device.destroy(slot.end);
        slot = Slot{};
    }
}

void GpuFrameTimer::beginFrame(Device& device, CommandList& commands) {
    m_current = m_next;
    m_next = (m_next + 1) % kFrames;
    Slot& slot = m_slots[m_current];
    if (slot.pending) {
        const auto begin = device.timestampNanoseconds(slot.begin);
        const auto end = device.timestampNanoseconds(slot.end);
        if (begin && end) {
            // The clock is monotonic, but guard against a driver handing back
            // a stale begin value, which would wrap around as unsigned.
            const u64 nanoseconds = *end >= *begin ? *end - *begin : 0;
            m_lastMilliseconds = static_cast<f64>(nanoseconds) / 1'000'000.0;
        } else {
            ++m_missed; // Overwriting it now drops this measurement.
        }
        slot.pending = false;
    }
    commands.writeTimestamp(slot.begin);
}

void GpuFrameTimer::endFrame(CommandList& commands) {
    Slot& slot = m_slots[m_current];
    commands.writeTimestamp(slot.end);
    slot.pending = true;
}

} // namespace vxe::rhi
