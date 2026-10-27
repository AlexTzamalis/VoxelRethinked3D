#pragma once

// Helpers shared by the engine unit tests.

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>

#include <doctest/doctest.h>

#include <cstdlib>
#include <format>
#include <string>

namespace vxe::test {

// Thrown by the test assert handler so a failed assert unwinds back into the
// test instead of aborting the whole test run.
struct AssertFired {
    std::string expression;
    std::string message;
    u32 line = 0;
};

[[noreturn]] inline void throwingAssertHandler(const AssertInfo& info) {
    throw AssertFired{.expression = std::string(info.expression),
                      .message = std::string(info.message),
                      .line = info.location.line()};
}

// Installs the throwing assert handler for one scope and restores the
// previous handler afterwards, so tests cannot leak it into each other.
class ScopedThrowingAssertHandler {
public:
    ScopedThrowingAssertHandler()
        : m_previous(setAssertHandler(&throwingAssertHandler)) {}
    ScopedThrowingAssertHandler(const ScopedThrowingAssertHandler&) = delete;
    ScopedThrowingAssertHandler& operator=(const ScopedThrowingAssertHandler&) = delete;
    ScopedThrowingAssertHandler(ScopedThrowingAssertHandler&&) = delete;
    ScopedThrowingAssertHandler& operator=(ScopedThrowingAssertHandler&&) = delete;
    ~ScopedThrowingAssertHandler() { setAssertHandler(m_previous); }

private:
    AssertHandler m_previous;
};

// Real windows and GPU contexts need a display, which CI machines do not
// have. Set VXE_GUI_TESTS=1 to run those tests locally.
inline bool guiTestsEnabled() {
#ifdef _MSC_VER
    // MSVC deprecates getenv in favor of _dupenv_s, which allocates the copy.
    char* value = nullptr;
    usize length = 0;
    const bool enabled =
        _dupenv_s(&value, &length, "VXE_GUI_TESTS") == 0 && value != nullptr && value[0] == '1';
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory): _dupenv_s uses
                      // malloc.
    return enabled;
#else
    const char* value = std::getenv("VXE_GUI_TESTS"); // NOLINT(concurrency-mt-unsafe): read once, no setenv.
    return value != nullptr && value[0] == '1';
#endif
}

} // namespace vxe::test

// Lets doctest print engine types in failure messages instead of "{?}", by
// reusing their std::formatter.
namespace doctest {

template <>
struct StringMaker<vxe::ChunkPos> {
    static String convert(const vxe::ChunkPos& value) { return std::format("{}", value).c_str(); }
};
template <>
struct StringMaker<vxe::BlockPos> {
    static String convert(const vxe::BlockPos& value) { return std::format("{}", value).c_str(); }
};
template <>
struct StringMaker<vxe::LocalPos> {
    static String convert(const vxe::LocalPos& value) { return std::format("{}", value).c_str(); }
};

} // namespace doctest
