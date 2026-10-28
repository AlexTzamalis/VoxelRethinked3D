#pragma once

#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>

namespace vxe::platform {

// Caps the frame rate (Section 8.5, setting Video.FpsLimit).
//
// Call wait() once per frame, after presenting. It keeps a fixed schedule of
// frame start times, so small delays in one frame are made up in the next
// instead of adding up, and the average matches the target closely. The wait
// itself uses SDL's precise delay, which sleeps for most of the time and spins
// only for the last fraction of a millisecond: plain sleeps on Windows can
// overshoot by a whole millisecond, which would make 500 FPS caps useless,
// while spinning the whole time would burn a CPU core.
//
// Thread safety: owned by the thread running the frame loop.
class FrameLimiter {
public:
    // Waits until the next frame may start. `framesPerSecond` 0 or less means
    // no limit: the call returns at once and forgets the schedule.
    void wait(f64 framesPerSecond);

private:
    TimePoint m_next{};
    bool m_scheduled = false;
};

} // namespace vxe::platform
