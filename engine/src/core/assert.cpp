#include "vxe/core/assert.hpp"

#include <vxe/core/log.hpp>

#include <atomic>
#include <cstdlib>

#ifdef _WIN32
// Only for IsDebuggerPresent. Keeping windows.h out of every other file avoids
// its macros (min, max, near, far) leaking into engine code.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace vxe {
namespace {

// Breaks into the debugger if one is attached, so the failing line is shown
// with its call stack. Without a debugger it does nothing and the program aborts.
void breakIntoDebuggerIfAttached() noexcept {
#ifdef _WIN32
    if (IsDebuggerPresent() != 0) {
        __debugbreak();
    }
#endif
}

void defaultAssertHandler(const AssertInfo& info) {
    VXE_LOG_CRITICAL(log::assertion, "Assertion failed: {}\n  Message: {}\n  At: {}:{} in {}",
                     info.expression, info.message, info.location.file_name(), info.location.line(),
                     info.location.function_name());
    log::flush();
    breakIntoDebuggerIfAttached();
}

// nullptr means the default handler.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): process wide hook, set at startup.
std::atomic<AssertHandler> currentAssertHandler{nullptr};

} // namespace

AssertHandler setAssertHandler(AssertHandler handler) noexcept {
    return currentAssertHandler.exchange(handler, std::memory_order_acq_rel);
}

namespace detail {

void assertFailed(std::string_view expression, std::string_view message, std::source_location location) {
    const AssertInfo info{.expression = expression, .message = message, .location = location};
    const AssertHandler handler = currentAssertHandler.load(std::memory_order_acquire);
    if (handler != nullptr) {
        handler(info); // May throw in tests, which unwinds out of here.
    } else {
        defaultAssertHandler(info);
    }
    std::abort();
}

} // namespace detail
} // namespace vxe
