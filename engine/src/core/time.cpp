#include "vxe/core/time.hpp"

#include <algorithm>

namespace vxe {

FrameTime FrameTimer::tick(TimePoint now) noexcept {
    if (!m_started) {
        // The first frame has no previous frame, so its delta is zero rather
        // than the time since some arbitrary point in the past.
        m_started = true;
        m_previous = now;
        m_current = FrameTime{};
        return m_current;
    }

    const Duration raw = std::max(now - m_previous, Duration::zero());
    const Duration clamped = std::min(raw, m_maxDelta);
    m_previous = now;

    m_current.rawDelta = raw;
    m_current.delta = clamped;
    m_current.deltaSeconds = toSeconds(clamped);
    m_current.elapsedSeconds += m_current.deltaSeconds;
    ++m_current.frameIndex;
    return m_current;
}

} // namespace vxe
