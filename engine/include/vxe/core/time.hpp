#pragma once

#include <vxe/core/types.hpp>

#include <chrono>

// Time keeping for the main loop, profiling and timeouts.
//
// All engine time comes from one monotonic clock (std::chrono::steady_clock,
// which is QueryPerformanceCounter on Windows and CLOCK_MONOTONIC on Linux).
// It never jumps when the user changes the system time, unlike system_clock.
namespace vxe {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

[[nodiscard]] constexpr f64 toSeconds(Duration duration) noexcept {
    return std::chrono::duration<f64>(duration).count();
}
[[nodiscard]] constexpr f64 toMilliseconds(Duration duration) noexcept {
    return std::chrono::duration<f64, std::milli>(duration).count();
}
[[nodiscard]] constexpr Duration fromSeconds(f64 seconds) noexcept {
    return std::chrono::duration_cast<Duration>(std::chrono::duration<f64>(seconds));
}

// Measures the time since it was started or last restarted.
class Stopwatch {
public:
    Stopwatch() noexcept
        : m_start(Clock::now()) {}

    void restart() noexcept { m_start = Clock::now(); }
    [[nodiscard]] Duration elapsed() const noexcept { return Clock::now() - m_start; }
    [[nodiscard]] f64 elapsedMilliseconds() const noexcept { return toMilliseconds(elapsed()); }

private:
    TimePoint m_start;
};

// Writes the time between its construction and destruction into `output`.
//   Duration meshTime{};
//   { ScopedTimer timer(meshTime); meshChunk(...); }
// For profiling with Tracy, use VXE_PROFILE_SCOPE instead (E1.6).
class ScopedTimer {
public:
    explicit ScopedTimer(Duration& output) noexcept
        : m_output(output) {}
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&) = delete;
    ScopedTimer& operator=(ScopedTimer&&) = delete;
    ~ScopedTimer() { m_output = m_stopwatch.elapsed(); }

private:
    Duration& m_output;
    Stopwatch m_stopwatch;
};

// The timing of one frame, handed to everything that runs per frame.
struct FrameTime {
    Duration delta{};         // Time since the previous frame, after clamping.
    Duration rawDelta{};      // Time since the previous frame as measured.
    f64 deltaSeconds = 0.0;   // delta in seconds, the form gameplay code wants.
    f64 elapsedSeconds = 0.0; // Sum of all clamped deltas since the timer started.
    u64 frameIndex = 0;       // 0 for the first frame.
};

// Produces a FrameTime once per frame.
//
// A frame can take seconds when the program was stopped in a debugger, the
// window was dragged or the OS stalled. Feeding such a delta to camera or UI
// animation makes everything jump, so the delta is clamped to `maxDelta`
// (rawDelta keeps the measured value for the frame time graph).
//
// Thread safety: none. The main thread owns the frame timer.
class FrameTimer {
public:
    explicit FrameTimer(Duration maxDelta = std::chrono::milliseconds(250)) noexcept
        : m_maxDelta(maxDelta) {}

    // Starts a new frame at the current time.
    FrameTime tick() noexcept { return tick(Clock::now()); }

    // Starts a new frame at `now`. Exists so tests can drive time by hand.
    FrameTime tick(TimePoint now) noexcept;

    [[nodiscard]] Duration maxDelta() const noexcept { return m_maxDelta; }
    void setMaxDelta(Duration maxDelta) noexcept { m_maxDelta = maxDelta; }

private:
    Duration m_maxDelta;
    TimePoint m_previous{};
    bool m_started = false;
    FrameTime m_current{};
};

} // namespace vxe
