#pragma once

#include <source_location>
#include <string_view>

// Assertion macros for programmer errors (Section 23.4 of the plan).
//
//   VXE_ASSERT(condition, "message")  Checked only when asserts are enabled
//                                     (Debug builds by default). When disabled
//                                     the condition is not evaluated at all, so
//                                     it may be expensive.
//   VXE_VERIFY(condition, "message")  Always checked, in every build. Use it for
//                                     invariants whose violation would corrupt
//                                     data (for example save files).
//   VXE_UNREACHABLE()                 Marks code that can never run. Fails like
//                                     an assert when asserts are enabled, and is
//                                     an optimizer hint otherwise.
//
// Expected failures (a missing file, a broken texture pack) are not asserts.
// They are returned as vxe::Result values instead.

// Asserts follow NDEBUG by default, which CMake defines for Release,
// RelWithDebInfo and MinSizeRel. Define VXE_ENABLE_ASSERTS to 0 or 1 to override.
#ifndef VXE_ENABLE_ASSERTS
#ifdef NDEBUG
#define VXE_ENABLE_ASSERTS 0
#else
#define VXE_ENABLE_ASSERTS 1
#endif
#endif

namespace vxe {

// Everything known about one failed assertion, handed to the assert handler.
struct AssertInfo {
    std::string_view expression; // The condition as written in the source.
    std::string_view message;    // The message passed to the macro.
    std::source_location location;
};

// Called when an assertion fails. The default handler logs the failure, breaks
// into an attached debugger and lets the program abort. If a handler returns,
// the program aborts as well. A handler may instead throw to unwind the stack;
// unit tests use this to check that an assert fired. Throwing out of a
// noexcept function still terminates, which is the intended behavior there.
using AssertHandler = void (*)(const AssertInfo& info);

// Installs a new assert handler and returns the previous one. Passing nullptr
// restores the default handler. Thread safe, but meant to be called once at
// startup or around a test.
AssertHandler setAssertHandler(AssertHandler handler) noexcept;

namespace detail {

// Reports a failed assertion and never returns. Kept out of line so the macros
// expand to a single compare and a call, which keeps hot code small.
[[noreturn]] void assertFailed(std::string_view expression, std::string_view message,
                               std::source_location location = std::source_location::current());

} // namespace detail
} // namespace vxe

#define VXE_VERIFY(condition, message)                                                                       \
    do {                                                                                                     \
        if (!(condition)) [[unlikely]] {                                                                     \
            ::vxe::detail::assertFailed(#condition, message);                                                \
        }                                                                                                    \
    } while (false)

#if VXE_ENABLE_ASSERTS
#define VXE_ASSERT(condition, message) VXE_VERIFY(condition, message)
#define VXE_UNREACHABLE() ::vxe::detail::assertFailed("unreachable", "Reached code marked as unreachable")
#else
// sizeof keeps the condition compiling (so it cannot rot) without evaluating it.
#define VXE_ASSERT(condition, message)                                                                       \
    do {                                                                                                     \
        static_cast<void>(sizeof(condition));                                                                \
        static_cast<void>(sizeof(message));                                                                  \
    } while (false)
#if defined(_MSC_VER) && !defined(__clang__)
#define VXE_UNREACHABLE() __assume(false)
#else
#define VXE_UNREACHABLE() __builtin_unreachable()
#endif
#endif
