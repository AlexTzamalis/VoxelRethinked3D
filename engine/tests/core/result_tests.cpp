#include "../test_helpers.hpp"

#include <vxe/core/error.hpp>
#include <vxe/core/result.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>

using vxe::Error;
using vxe::ErrorCode;
using vxe::Result;

namespace {

Result<int> parsePositive(int input) {
    if (input <= 0) {
        return Error(ErrorCode::InvalidArgument, "must be positive");
    }
    return input;
}

// Uses VXE_TRY to pass the error of the first failing step up unchanged.
Result<int> sumOfPositives(int a, int b) {
    VXE_TRY(parsePositive(a));
    VXE_TRY(parsePositive(b));
    return a + b;
}

Result<void> requireEven(int input) {
    if (input % 2 != 0) {
        return Error(ErrorCode::InvalidArgument, "odd");
    }
    return {};
}

Result<void> requireAllEven(int a, int b) {
    VXE_TRY(requireEven(a));
    VXE_TRY(requireEven(b));
    return {};
}

} // namespace

TEST_CASE("Result holds a value") {
    const Result<int> result = parsePositive(5);
    REQUIRE(result.hasValue());
    CHECK_FALSE(result.hasError());
    CHECK(static_cast<bool>(result));
    CHECK(result.value() == 5);
    CHECK(*result == 5);
}

TEST_CASE("Result holds an error") {
    const Result<int> result = parsePositive(-1);
    REQUIRE(result.hasError());
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(result.error().code() == ErrorCode::InvalidArgument);
    CHECK(result.error().message() == "must be positive");
}

TEST_CASE("valueOr returns the fallback only on error") {
    CHECK(parsePositive(3).valueOr(0) == 3);
    CHECK(parsePositive(-3).valueOr(42) == 42);
}

TEST_CASE("Result works with move only values") {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
    REQUIRE(result.hasValue());
    CHECK(*result.value() == 7);

    const std::unique_ptr<int> taken = std::move(result).value();
    CHECK(*taken == 7);
}

TEST_CASE("Result arrow operator reaches value members") {
    const Result<std::string> result = std::string("voxel");
    CHECK(result->size() == 5);
}

TEST_CASE("Result<void> success and failure") {
    const Result<void> ok = requireEven(4);
    CHECK(ok.hasValue());

    const Result<void> failed = requireEven(3);
    REQUIRE(failed.hasError());
    CHECK(failed.error().message() == "odd");
}

TEST_CASE("VXE_TRY propagates the first error") {
    CHECK(sumOfPositives(2, 3).value() == 5);

    const Result<int> failed = sumOfPositives(2, -3);
    REQUIRE(failed.hasError());
    CHECK(failed.error().code() == ErrorCode::InvalidArgument);

    CHECK(requireAllEven(2, 4).hasValue());
    CHECK(requireAllEven(2, 5).hasError());
}

TEST_CASE("Error records where it was created") {
    const Error error(ErrorCode::NotFound, "pack.toml missing");
    CHECK(error.location().line() == __LINE__ - 1);
    CHECK(std::string(error.location().file_name()).find("result_tests.cpp") != std::string::npos);
}

TEST_CASE("Error context and description") {
    const Error error(ErrorCode::NotFound, "pack.toml missing");
    const Error withContext = error.withContext("Loading pack 'MyPack'");
    CHECK(withContext.message() == "Loading pack 'MyPack': pack.toml missing");
    CHECK(withContext.code() == ErrorCode::NotFound);
    CHECK(withContext.location().line() == error.location().line());
    CHECK(error.message() == "pack.toml missing"); // The original is unchanged.

    const std::string description = withContext.describe();
    CHECK(description.starts_with("NotFound: Loading pack 'MyPack': pack.toml missing (result_tests.cpp:"));

    CHECK(Error(ErrorCode::Timeout).describe().starts_with("Timeout (result_tests.cpp:"));
    CHECK(vxe::nameOf(ErrorCode::VersionMismatch) == "VersionMismatch");
}

#if VXE_ENABLE_ASSERTS
TEST_CASE("Accessing the wrong side of a Result asserts") {
    const vxe::test::ScopedThrowingAssertHandler handler;
    const Result<int> value = 1;
    const Result<int> error = Error(ErrorCode::Unknown);
    CHECK_THROWS_AS(static_cast<void>(value.error()), vxe::test::AssertFired);
    CHECK_THROWS_AS(static_cast<void>(error.value()), vxe::test::AssertFired);

    const Result<void> ok;
    CHECK_THROWS_AS(static_cast<void>(ok.error()), vxe::test::AssertFired);
}
#endif
