#pragma once

#include <vxe/core/types.hpp>

#include <ankerl/unordered_dense.h>

#include <functional>
#include <string>
#include <string_view>

// Engine hash containers.
//
// ankerl::unordered_dense stores its elements in one contiguous vector and
// uses open addressing for the index, which makes lookups and iteration much
// faster than std::unordered_map (no node per element, no pointer chasing).
// The price: inserting or erasing may move elements, so pointers and
// references into the map are not stable. Store values by pointer (for
// example chunks from a PoolAllocator) when they must not move.
namespace vxe {

// The default hash is ankerl's own (wyhash based) for builtin types and
// strings, and std::hash for everything else.
template <typename Key, typename Value, typename Hash = ankerl::unordered_dense::hash<Key>,
          typename Equal = std::equal_to<Key>>
using HashMap = ankerl::unordered_dense::map<Key, Value, Hash, Equal>;

template <typename Key, typename Hash = ankerl::unordered_dense::hash<Key>,
          typename Equal = std::equal_to<Key>>
using HashSet = ankerl::unordered_dense::set<Key, Hash, Equal>;

// Hashes std::string, std::string_view and string literals the same way, and
// is marked transparent, so a StringMap can be searched with a string_view
// without building a temporary std::string.
struct StringHash {
    using is_transparent = void; // NOLINT(readability-identifier-naming): required by the library.
    using is_avalanching = void; // NOLINT(readability-identifier-naming): ankerl skips its extra mixing step.

    [[nodiscard]] u64 operator()(std::string_view text) const noexcept {
        return ankerl::unordered_dense::hash<std::string_view>{}(text);
    }
};

// A map with std::string keys that accepts std::string_view lookups.
template <typename Value>
using StringMap = HashMap<std::string, Value, StringHash, std::equal_to<>>;

} // namespace vxe
