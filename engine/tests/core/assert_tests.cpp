#include "../test_helpers.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>

#include <doctest/doctest.h>

using vxe::test::AssertFired;
using ScopedThrowingHandler = vxe::test::ScopedThrowingAssertHandler;

TEST_CASE("Fixed width aliases have the expected sizes") {
    static_assert(sizeof(vxe::u8) == 1 && sizeof(vxe::u16) == 2 && sizeof(vxe::u32) == 4 &&
                  sizeof(vxe::u64) == 8);
    static_assert(sizeof(vxe::i8) == 1 && sizeof(vxe::i16) == 2 && sizeof(vxe::i32) == 4 &&
                  sizeof(vxe::i64) == 8);
    static_assert(sizeof(vxe::usize) == 8 && sizeof(vxe::isize) == 8);
}

TEST_CASE("VXE_VERIFY passes on true and fires on false in every build") {
    const ScopedThrowingHandler handler;

    CHECK_NOTHROW([] { VXE_VERIFY(1 + 1 == 2, "math works"); }());

    // A flag instead of FAIL() after the call: the optimizer knows the call
    // never returns, and code after it would be an unreachable code warning.
    bool fired = false;
    try {
        [] { VXE_VERIFY(1 + 1 == 3, "math is broken"); }();
    } catch (const AssertFired& info) {
        fired = true;
        CHECK(info.expression == "1 + 1 == 3");
        CHECK(info.message == "math is broken");
        CHECK(info.line > 0);
    }
    CHECK(fired);
}

TEST_CASE("VXE_ASSERT follows VXE_ENABLE_ASSERTS") {
    const ScopedThrowingHandler handler;
    int evaluations = 0;
    auto failingCondition = [&evaluations] {
        ++evaluations;
        return false;
    };

#if VXE_ENABLE_ASSERTS
    CHECK_THROWS_AS([&] { VXE_ASSERT(failingCondition(), "fires in debug"); }(), AssertFired);
    CHECK(evaluations == 1);
#else
    // Disabled asserts must not even evaluate the condition.
    VXE_ASSERT(failingCondition(), "compiled out");
    CHECK(evaluations == 0);
#endif
}

#if VXE_ENABLE_ASSERTS
TEST_CASE("VXE_UNREACHABLE fires when asserts are enabled") {
    const ScopedThrowingHandler handler;
    CHECK_THROWS_AS([] { VXE_UNREACHABLE(); }(), AssertFired);
}
#endif

TEST_CASE("setAssertHandler returns the previous handler") {
    const vxe::AssertHandler original = vxe::setAssertHandler(&vxe::test::throwingAssertHandler);
    CHECK(vxe::setAssertHandler(original) == &vxe::test::throwingAssertHandler);
}
