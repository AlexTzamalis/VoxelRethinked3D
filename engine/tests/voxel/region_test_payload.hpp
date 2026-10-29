#pragma once

#include <vxe/core/types.hpp>

#include <cstring>
#include <vector>

namespace vxe::test {

// A chunk payload stand in whose bytes and size follow from (round, slot),
// so a reader can tell exactly which commit wrote it. Shared by the region
// tests and the crash test writer process.
inline std::vector<std::byte> regionTestPayload(u64 round, u16 slot) {
    const usize size = 16 + static_cast<usize>(((round * 977) + (u64{slot} * 131)) % 2500);
    std::vector<std::byte> bytes(size);
    std::memcpy(bytes.data(), &round, sizeof(round));
    std::memcpy(bytes.data() + sizeof(round), &slot, sizeof(slot));
    for (usize i = 10; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(((round * 7) + (u64{slot} * 3) + (i / 5)) & 0xFFU);
    }
    return bytes;
}

// The round a payload was written in, read from its first bytes.
inline u64 regionTestRound(const std::vector<std::byte>& bytes) {
    u64 round = 0;
    if (bytes.size() >= sizeof(round)) {
        std::memcpy(&round, bytes.data(), sizeof(round));
    }
    return round;
}

} // namespace vxe::test
