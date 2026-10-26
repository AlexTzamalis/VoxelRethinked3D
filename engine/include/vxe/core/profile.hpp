#pragma once

// Profiling macros on top of the Tracy profiler (Section 18.3 of the plan).
//
// Profiling is compiled in only when VXE_ENABLE_PROFILING is 1, which the
// relwithdebinfo preset sets through the CMake option of the same name. In
// every other build the macros expand to nothing, so they cost nothing and
// may be sprinkled freely over hot code.
//
// The Tracy client is built "on demand": it records nothing until the Tracy
// profiler connects, so a profiling build also runs at full speed normally.
//
//   void meshChunk(...) {
//       VXE_PROFILE_FUNCTION();            // Zone named after the function.
//       { VXE_PROFILE_SCOPE("Greedy merge"); ... }
//   }
//   VXE_PROFILE_FRAME();                   // Once per frame, after present.
//   VXE_PROFILE_PLOT("Resident chunks", static_cast<double>(count));
//
// Zone and plot names must be string literals: Tracy keeps the pointer.

#ifndef VXE_ENABLE_PROFILING
#define VXE_ENABLE_PROFILING 0
#endif

#if VXE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>

#define VXE_PROFILE_SCOPE(name) ZoneScopedN(name)
#define VXE_PROFILE_FUNCTION() ZoneScoped
#define VXE_PROFILE_FRAME() FrameMark
#define VXE_PROFILE_FRAME_NAMED(name) FrameMarkNamed(name)
#define VXE_PROFILE_PLOT(name, value) TracyPlot(name, value)
#define VXE_PROFILE_MESSAGE(text, length) TracyMessage(text, length)
#else
#define VXE_PROFILE_SCOPE(name) static_cast<void>(0)
#define VXE_PROFILE_FUNCTION() static_cast<void>(0)
#define VXE_PROFILE_FRAME() static_cast<void>(0)
#define VXE_PROFILE_FRAME_NAMED(name) static_cast<void>(0)
#define VXE_PROFILE_PLOT(name, value) static_cast<void>(0)
#define VXE_PROFILE_MESSAGE(text, length) static_cast<void>(0)
#endif

namespace vxe::profiling {

// True when this build contains the profiler client.
[[nodiscard]] constexpr bool isEnabled() noexcept {
    return VXE_ENABLE_PROFILING != 0;
}

// Names the calling thread, for example "Worker 3". The name shows up in
// Tracy (when profiling is enabled) and in debuggers (always), which makes
// the thread list of a crash dump readable. `name` must stay valid for the
// life of the thread; string literals are the normal case.
void setThreadName(const char* name) noexcept;

} // namespace vxe::profiling
