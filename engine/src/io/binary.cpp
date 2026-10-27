#include "vxe/io/binary.hpp"

namespace vxe::io {
namespace {

// A u64 needs at most ten 7 bit groups.
constexpr int kMaxVarintBytes = 10;

} // namespace

void BinaryWriter::writeVarU64(u64 value) {
    while (value >= 0x80U) {
        writeU8(static_cast<u8>((value & 0x7FU) | 0x80U)); // Low 7 bits, "more follows" flag.
        value >>= 7U;
    }
    writeU8(static_cast<u8>(value));
}

void BinaryWriter::writeVarI64(i64 value) {
    // ZigZag: (value << 1) ^ (value >> 63) with the shifts done on the bit
    // pattern, so the sign bit becomes the lowest bit.
    const auto bits = static_cast<u64>(value);
    writeVarU64((bits << 1U) ^ (value < 0 ? ~u64{0} : u64{0}));
}

void BinaryWriter::writeString(std::string_view text) {
    writeVarU64(text.size());
    writeBytes(std::as_bytes(std::span(text.data(), text.size())));
}

void BinaryWriter::writeBytes(std::span<const std::byte> bytes) {
    m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
}

u64 BinaryReader::readVarU64() noexcept {
    u64 value = 0;
    for (int i = 0; i < kMaxVarintBytes; ++i) {
        const u8 byte = readU8();
        if (m_failed) {
            return 0;
        }
        const auto shift = static_cast<unsigned>(7 * i);
        // The tenth byte may only carry the single remaining bit of a u64.
        if (i == kMaxVarintBytes - 1 && byte > 1U) {
            m_failed = true;
            return 0;
        }
        value |= static_cast<u64>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
    }
    m_failed = true; // More than ten bytes: not a valid u64.
    return 0;
}

i64 BinaryReader::readVarI64() noexcept {
    const u64 zigzag = readVarU64();
    const u64 bits = (zigzag >> 1U) ^ ((zigzag & 1U) != 0 ? ~u64{0} : u64{0});
    return static_cast<i64>(bits);
}

std::string BinaryReader::readString() {
    const u64 length = readVarU64();
    if (m_failed || length > remaining()) {
        m_failed = true;
        return {};
    }
    const std::span<const std::byte> bytes = readBytes(static_cast<usize>(length));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::span<const std::byte> BinaryReader::readBytes(usize count) noexcept {
    if (m_failed || count > remaining()) {
        m_failed = true;
        return {};
    }
    const std::span<const std::byte> result = m_bytes.subspan(m_position, count);
    m_position += count;
    return result;
}

} // namespace vxe::io
