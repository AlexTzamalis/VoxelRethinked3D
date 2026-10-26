#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/error.hpp>

#include <type_traits>
#include <utility>
#include <variant>

namespace vxe {

// A small stand in for C++23 std::expected. It holds either a value or an
// error, never both. Using it makes failure part of the function signature,
// so callers cannot forget that something can go wrong ([[nodiscard]] makes
// ignoring a Result a compiler warning).
//
//   Result<Texture> loadTexture(std::string_view key);
//   auto texture = loadTexture("stone");
//   if (!texture) { VXE_LOG_WARN(..., "{}", texture.error().describe()); }
//
// Accessing value() on an error (or error() on a value) is a programmer error
// and fires VXE_ASSERT.
template <typename T, typename E = Error>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>,
                  "Result<T&> is not supported, use a pointer or std::reference_wrapper");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, std::remove_cv_t<E>>,
                  "Result needs distinct value and error types to know which one it holds");

public:
    using ValueType = T;
    using ErrorType = E;

    // Implicit on purpose: `return value;` and `return Error(...);` both work.
    // NOLINTBEGIN(google-explicit-constructor, hicpp-explicit-conversions)
    Result(const T& value)
        : m_storage(std::in_place_index<0>, value) {}
    Result(T&& value)
        : m_storage(std::in_place_index<0>, std::move(value)) {}
    Result(const E& error)
        : m_storage(std::in_place_index<1>, error) {}
    Result(E&& error)
        : m_storage(std::in_place_index<1>, std::move(error)) {}
    // NOLINTEND(google-explicit-constructor, hicpp-explicit-conversions)

    [[nodiscard]] bool hasValue() const noexcept { return m_storage.index() == 0; }
    [[nodiscard]] bool hasError() const noexcept { return m_storage.index() == 1; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T& value() & {
        VXE_ASSERT(hasValue(), "Result has no value");
        return *std::get_if<0>(&m_storage);
    }
    [[nodiscard]] const T& value() const& {
        VXE_ASSERT(hasValue(), "Result has no value");
        return *std::get_if<0>(&m_storage);
    }
    [[nodiscard]] T&& value() && {
        VXE_ASSERT(hasValue(), "Result has no value");
        return std::move(*std::get_if<0>(&m_storage));
    }

    [[nodiscard]] E& error() & {
        VXE_ASSERT(hasError(), "Result has no error");
        return *std::get_if<1>(&m_storage);
    }
    [[nodiscard]] const E& error() const& {
        VXE_ASSERT(hasError(), "Result has no error");
        return *std::get_if<1>(&m_storage);
    }
    [[nodiscard]] E&& error() && {
        VXE_ASSERT(hasError(), "Result has no error");
        return std::move(*std::get_if<1>(&m_storage));
    }

    // Shorthand for value(), so a Result reads like a pointer after the check.
    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }

    // Returns the value, or `fallback` when this holds an error.
    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) const& {
        return hasValue() ? value() : static_cast<T>(std::forward<U>(fallback));
    }
    template <typename U>
    [[nodiscard]] T valueOr(U&& fallback) && {
        return hasValue() ? std::move(*this).value() : static_cast<T>(std::forward<U>(fallback));
    }

private:
    // get_if instead of std::get: the index is already checked by the assert,
    // and std::get would add a second check that throws, which engine code
    // never does.
    std::variant<T, E> m_storage;
};

// Result for operations that either succeed with nothing to return or fail.
//   Result<void> saveWorld();   ...   return {};   or   return Error(...);
template <typename E>
class [[nodiscard]] Result<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    // A default constructed Result<void> means success.
    Result() = default;
    // NOLINTBEGIN(google-explicit-constructor, hicpp-explicit-conversions)
    Result(const E& error)
        : m_storage(std::in_place_index<1>, error) {}
    Result(E&& error)
        : m_storage(std::in_place_index<1>, std::move(error)) {}
    // NOLINTEND(google-explicit-constructor, hicpp-explicit-conversions)

    [[nodiscard]] bool hasValue() const noexcept { return m_storage.index() == 0; }
    [[nodiscard]] bool hasError() const noexcept { return m_storage.index() == 1; }
    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] E& error() & {
        VXE_ASSERT(hasError(), "Result has no error");
        return *std::get_if<1>(&m_storage);
    }
    [[nodiscard]] const E& error() const& {
        VXE_ASSERT(hasError(), "Result has no error");
        return *std::get_if<1>(&m_storage);
    }
    [[nodiscard]] E&& error() && {
        VXE_ASSERT(hasError(), "Result has no error");
        return std::move(*std::get_if<1>(&m_storage));
    }

private:
    // monostate is the success state, so E needs no default constructor.
    std::variant<std::monostate, E> m_storage;
};

} // namespace vxe

// Propagates an error to the caller: evaluates `expression` (a Result) and, if
// it holds an error, returns that error from the current function. The current
// function must return a Result with the same error type.
//
//   Result<void> loadPack(const Path& folder) {
//       VXE_TRY(readManifest(folder));
//       ...
//   }
#define VXE_TRY(expression)                                                                                  \
    do {                                                                                                     \
        auto&& vxeTryResult = (expression);                                                                  \
        if (!vxeTryResult.hasValue()) [[unlikely]] {                                                         \
            return std::move(vxeTryResult).error();                                                          \
        }                                                                                                    \
    } while (false)
