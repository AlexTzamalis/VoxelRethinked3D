#pragma once

#include <vxe/core/result.hpp>
#include <vxe/rhi/device.hpp>

#include <memory>

namespace vxe::platform {
class Window;
}

// The OpenGL 4.6 backend (Section 7.5). This header is the only part of the
// backend other modules see: it never exposes a GL type, so nothing outside
// engine/src/rhi_gl needs GL headers (ADR-003).
namespace vxe::rhi {

struct GlDeviceOptions {
    // Report driver messages (KHR_debug) to the log and count them in
    // DeviceStats. Costs a little CPU per GL call on some drivers, so it
    // defaults to on in builds with asserts and off otherwise.
#ifdef NDEBUG
    bool debugOutput = false;
#else
    bool debugOutput = true;
#endif
    // Deliver each message on the main thread inside the GL call that caused
    // it, so a breakpoint in the log shows the culprit in the call stack.
    // Slower, because the driver cannot defer work.
    bool synchronousDebugOutput = true;
};

// Creates the OpenGL device for a window created with GraphicsApi::OpenGL.
// Loads the GL functions, checks for OpenGL 4.5 or newer, detects optional
// features and logs them. The window (and its context) must outlive the
// device; destroy the device first.
[[nodiscard]] Result<std::unique_ptr<Device>> createGlDevice(platform::Window& window,
                                                             const GlDeviceOptions& options = {});

} // namespace vxe::rhi
