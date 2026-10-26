#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/core/types.hpp>

#include <compare>
#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>

// Debug builds remember the text of every StringId so it can be printed and
// so two different strings with the same hash are caught. Release builds keep
// only the 64 bit hash. Follows the assert setting by default.
#ifndef VXE_STRING_ID_DEBUG_NAMES
#define VXE_STRING_ID_DEBUG_NAMES VXE_ENABLE_ASSERTS
#endif

namespace vxe {

namespace detail {
// Stores a copy of `text` under `hash` and returns a view of the stored copy,
// which lives until the program exits. Fails with VXE_VERIFY if a different
// text already has the same hash. Thread safe.
[[nodiscard]] std::string_view registerStringIdName(u64 hash, std::string_view text);
} // namespace detail

// An identifier made from a string, compared and hashed as a single u64.
//
// Used where the engine would otherwise compare strings in hot code: setting
// ids, action ids, texture keys, pass names. Comparing two StringIds is one
// integer compare instead of a string compare.
//
//   constexpr StringId kJump = "gameplay.jump"_sid; // Hashed at compile time.
//   StringId fromFile(nameReadFromToml);            // Hashed at runtime.
//
// The hash is FNV-1a so it can run at compile time. It is stable, but it is
// not meant for saving to disk: saves store real names (Section 9.3).
class StringId {
public:
    // The empty id, equal to StringId("").
    constexpr StringId()
        : StringId(std::string_view{}) {}

    // Not noexcept: in debug builds the registry may allocate.
    constexpr explicit StringId(std::string_view text)
        : m_hash(fnv1a64(text)) {
#if VXE_STRING_ID_DEBUG_NAMES
        // At compile time the text is a literal that lives forever. At runtime
        // it may be a temporary, so the registry keeps a permanent copy.
        if (std::is_constant_evaluated()) {
            m_debugName = text;
        } else {
            m_debugName = detail::registerStringIdName(m_hash, text);
        }
#endif
    }

    [[nodiscard]] constexpr u64 value() const noexcept { return m_hash; }
    [[nodiscard]] constexpr bool isEmpty() const noexcept { return m_hash == fnv1a64({}); }

    // The original text in debug builds, an empty view in release builds.
    // Meant for logs and debug UI only, never for logic.
    [[nodiscard]] constexpr std::string_view debugName() const noexcept {
#if VXE_STRING_ID_DEBUG_NAMES
        return m_debugName;
#else
        return {};
#endif
    }

    // Only the hash takes part in comparisons, never the debug name.
    [[nodiscard]] friend constexpr bool operator==(const StringId& lhs, const StringId& rhs) noexcept {
        return lhs.m_hash == rhs.m_hash;
    }
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const StringId& lhs,
                                                                    const StringId& rhs) noexcept {
        return lhs.m_hash <=> rhs.m_hash;
    }

private:
    u64 m_hash;
#if VXE_STRING_ID_DEBUG_NAMES
    std::string_view m_debugName;
#endif
};

inline namespace literals {
// "gameplay.jump"_sid creates a StringId at compile time.
[[nodiscard]] consteval StringId operator""_sid(const char* text, std::size_t length) {
    return StringId(std::string_view(text, length));
}
} // namespace literals

} // namespace vxe

// std::hash, so StringId works as a key in HashMap and std containers. FNV-1a
// is weak in its low bits, so the value is passed through hashCombine once.
template <>
struct std::hash<vxe::StringId> {
    [[nodiscard]] std::size_t operator()(const vxe::StringId& id) const noexcept {
        return static_cast<std::size_t>(vxe::hashCombine(0, id.value()));
    }
};
