#pragma once

// Helpers shared by the engine unit tests.

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>

#include <doctest/doctest.h>

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
