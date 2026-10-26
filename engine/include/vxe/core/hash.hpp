#pragma once

#include <vxe/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

// Hash functions.
//
// Two families with different jobs:
//   hashBytes / hashString   XXH3 (xxHash). Very fast on large inputs and of
//                            high quality. Use it for content hashes: cache
//                            keys of LOD tiles, golden test hashes, file data.
//                            Stable across platforms and versions, so results
//                            may be stored on disk.
//   fnv1a64                  FNV-1a. Slower and weaker, but constexpr, so it
//                            can hash identifiers at compile time (StringId).
namespace vxe {

// XXH3 64 bit hash of raw bytes.
[[nodiscard]] u64 hashBytes(std::span<const std::byte> bytes, u64 seed = 0) noexcept;

// XXH3 64 bit hash of the characters of a string (no terminator).
[[nodiscard]] u64 hashString(std::string_view text, u64 seed = 0) noexcept;

// XXH3 64 bit hash of a plain value's object representation. Only for types
// without padding, because padding bytes have unspecified values and would
// make equal values hash differently.
template <typename T>
[[nodiscard]] u64 hashValue(const T& value, u64 seed = 0) noexcept {
    static_assert(std::has_unique_object_representations_v<T>,
                  "hashValue needs a type without padding; hash its fields with hashCombine instead");
    return hashBytes(std::as_bytes(std::span(&value, 1)), seed);
}

// Mixes `value` into `seed`. Used to build the hash of a struct field by field:
//   u64 h = hashCombine(hashCombine(0, pos.x), pos.y);
// The constant and shifts are the 64 bit version of the well known
// boost::hash_combine, with an extra multiply for better avalanche.
[[nodiscard]] constexpr u64 hashCombine(u64 seed, u64 value) noexcept {
    constexpr u64 kGoldenRatio = 0x9e3779b97f4a7c15ULL;
    seed ^= value + kGoldenRatio + (seed << 6U) + (seed >> 2U);
    seed *= 0xff51afd7ed558ccdULL; // Finalizer constant from MurmurHash3.
    return seed ^ (seed >> 33U);
}

// 64 bit FNV-1a, usable at compile time.
[[nodiscard]] constexpr u64 fnv1a64(std::string_view text) noexcept {
    constexpr u64 kOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr u64 kPrime = 0x100000001b3ULL;
    u64 hash = kOffsetBasis;
    for (const char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= kPrime;
    }
    return hash;
}

} // namespace vxe
