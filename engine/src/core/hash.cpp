#include "vxe/core/hash.hpp"

#include <xxhash.h>

namespace vxe {

u64 hashBytes(std::span<const std::byte> bytes, u64 seed) noexcept {
    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
}

u64 hashString(std::string_view text, u64 seed) noexcept {
    return XXH3_64bits_withSeed(text.data(), text.size(), seed);
}

} // namespace vxe
