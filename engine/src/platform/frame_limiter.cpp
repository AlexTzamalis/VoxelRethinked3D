#include "vxe/platform/frame_limiter.hpp"

#include <SDL3/SDL_timer.h>

#include <chrono>

namespace vxe::platform {

void FrameLimiter::wait(f64 framesPerSecond) {
    if (framesPerSecond <= 0.0) {
        m_scheduled = false;
        return;
    }
    const Duration period = fromSeconds(1.0 / framesPerSecond);
    const TimePoint now = Clock::now();
    // Start a new schedule the first time, and whenever the frame loop fell a
    // whole frame behind (a loading hitch, a debugger break). Catching up on
    // every missed frame would then run frames back to back without waiting.
    if (!m_scheduled || now - m_next > period) {
        m_next = now;
        m_scheduled = true;
    }
    m_next += period;
    const Duration remaining = m_next - Clock::now();
    if (remaining > Duration::zero()) {
        SDL_DelayPrecise(
            static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count()));
    }
}

} // namespace vxe::platform
