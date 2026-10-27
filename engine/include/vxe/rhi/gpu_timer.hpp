#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>

#include <array>
#include <optional>

namespace vxe::rhi {

// Measures how long the GPU spends on each frame without ever waiting for it.
//
// Asking the GPU for a timestamp it has not written yet would stall the CPU
// until the GPU catches up, which destroys the very frame rate being
// measured. So the timer keeps a ring of kFrames query pairs: each frame
// writes a begin and an end timestamp into the next pair, and reads the pair
// written kFrames frames ago, whose results are normally ready by then
// (Section 7.5: "results read 3 frames later"). The reported time is
// therefore a few frames old, which is fine for graphs and the overlay.
//
// Thread safety: main thread only, like the device.
class GpuFrameTimer {
public:
    static constexpr usize kFrames = 4;

    GpuFrameTimer() = default;
    GpuFrameTimer(const GpuFrameTimer&) = delete;
    GpuFrameTimer& operator=(const GpuFrameTimer&) = delete;
    GpuFrameTimer(GpuFrameTimer&&) = delete;
    GpuFrameTimer& operator=(GpuFrameTimer&&) = delete;
    ~GpuFrameTimer() = default;

    // Creates the queries. Call destroy() before the device goes away.
    [[nodiscard]] Result<void> create(Device& device);
    void destroy(Device& device);

    // Reads the oldest pair if the GPU has finished it, then records the
    // begin timestamp of this frame. Record it first in the frame's commands.
    void beginFrame(Device& device, CommandList& commands);
    // Records the end timestamp. Record it last in the frame's commands.
    void endFrame(CommandList& commands);

    // GPU time of the most recently completed measurement, in milliseconds.
    [[nodiscard]] std::optional<f64> lastFrameMilliseconds() const noexcept { return m_lastMilliseconds; }
    // Frames whose results were still not ready after kFrames frames. A
    // steadily rising count means the GPU is more than kFrames frames behind.
    [[nodiscard]] u64 missedResults() const noexcept { return m_missed; }

private:
    struct Slot {
        QueryHandle begin;
        QueryHandle end;
        bool pending = false; // Written and not read yet.
    };

    std::array<Slot, kFrames> m_slots{};
    usize m_next = 0;
    usize m_current = 0;
    std::optional<f64> m_lastMilliseconds;
    u64 m_missed = 0;
};

} // namespace vxe::rhi
