#pragma once

#include <vxe/core/types.hpp>

#include <bit>
#include <concepts>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Binary serialization with a fixed byte order.
//
// Saves, region files, the LOD cache and network messages are read on other
// machines, so the format must never depend on the CPU. Everything is written
// little endian (the order of every CPU the game targets, so writing is a
// plain copy) and floats are written as their IEEE 754 bits. Structs are never
// copied as raw memory, because padding bytes and layout differ between
// compilers (Section 22: no memcpy of structs with padding).
namespace vxe::io {

namespace detail {

// Reverses the byte order of an unsigned integer (std::byteswap is C++23).
template <std::unsigned_integral T>
[[nodiscard]] constexpr T byteSwap(T value) noexcept {
    T result = 0;
    for (usize i = 0; i < sizeof(T); ++i) {
        result = static_cast<T>((result << 8U) | (value & 0xFFU));
        value = static_cast<T>(value >> 8U);
    }
    return result;
}

template <std::unsigned_integral T>
[[nodiscard]] constexpr T toLittleEndian(T value) noexcept {
    if constexpr (std::endian::native == std::endian::little || sizeof(T) == 1) {
        return value;
    } else {
        return byteSwap(value);
    }
}

} // namespace detail

// Appends values to a growing byte buffer. The buffer can be reused between
// messages (clear() keeps its capacity), so steady state writing does not
// allocate.
class BinaryWriter {
public:
    BinaryWriter() = default;
    explicit BinaryWriter(usize reserveBytes) { m_bytes.reserve(reserveBytes); }

    void writeU8(u8 value) { writeUnsigned(value); }
    void writeU16(u16 value) { writeUnsigned(value); }
    void writeU32(u32 value) { writeUnsigned(value); }
    void writeU64(u64 value) { writeUnsigned(value); }
    void writeI8(i8 value) { writeUnsigned(static_cast<u8>(value)); }
    void writeI16(i16 value) { writeUnsigned(static_cast<u16>(value)); }
    void writeI32(i32 value) { writeUnsigned(static_cast<u32>(value)); }
    void writeI64(i64 value) { writeUnsigned(static_cast<u64>(value)); }
    void writeF32(f32 value) { writeUnsigned(std::bit_cast<u32>(value)); }
    void writeF64(f64 value) { writeUnsigned(std::bit_cast<u64>(value)); }
    void writeBool(bool value) { writeU8(value ? 1 : 0); }

    // Variable length unsigned integer (LEB128): 7 bits per byte, small values
    // take one byte. Good for counts and ids that are usually small.
    void writeVarU64(u64 value);

    // Signed variable length integer. ZigZag maps small negative and positive
    // numbers to small unsigned ones (0, -1, 1, -2 ... become 0, 1, 2, 3 ...)
    // so they stay short.
    void writeVarI64(i64 value);

    // A string as a varint length followed by its bytes (no terminator).
    void writeString(std::string_view text);

    void writeBytes(std::span<const std::byte> bytes);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }
    [[nodiscard]] usize size() const noexcept { return m_bytes.size(); }
    [[nodiscard]] std::vector<std::byte> takeBytes() noexcept { return std::move(m_bytes); }
    void clear() noexcept { m_bytes.clear(); }

private:
    template <std::unsigned_integral T>
    void writeUnsigned(T value) {
        const T little = detail::toLittleEndian(value);
        const usize offset = m_bytes.size();
        m_bytes.resize(offset + sizeof(T));
        std::memcpy(m_bytes.data() + offset, &little, sizeof(T));
    }

    std::vector<std::byte> m_bytes;
};

// Reads values from a byte span written by BinaryWriter.
//
// Reading past the end, or an invalid varint, does not throw and does not
// read out of bounds: the reader enters a failed state, every later read
// returns zero (or empty), and ok() turns false. Code reads a whole record
// and checks ok() once at the end, which keeps parsing code short:
//   BinaryReader reader(bytes);
//   const u32 version = reader.readU32();
//   const std::string name = reader.readString();
//   if (!reader.ok()) { return Error(ErrorCode::ParseError, "truncated header"); }
//
// The span must outlive the reader. readString copies, readBytes does not.
class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> bytes) noexcept
        : m_bytes(bytes) {}

    [[nodiscard]] u8 readU8() noexcept { return readUnsigned<u8>(); }
    [[nodiscard]] u16 readU16() noexcept { return readUnsigned<u16>(); }
    [[nodiscard]] u32 readU32() noexcept { return readUnsigned<u32>(); }
    [[nodiscard]] u64 readU64() noexcept { return readUnsigned<u64>(); }
    [[nodiscard]] i8 readI8() noexcept { return static_cast<i8>(readUnsigned<u8>()); }
    [[nodiscard]] i16 readI16() noexcept { return static_cast<i16>(readUnsigned<u16>()); }
    [[nodiscard]] i32 readI32() noexcept { return static_cast<i32>(readUnsigned<u32>()); }
    [[nodiscard]] i64 readI64() noexcept { return static_cast<i64>(readUnsigned<u64>()); }
    [[nodiscard]] f32 readF32() noexcept { return std::bit_cast<f32>(readUnsigned<u32>()); }
    [[nodiscard]] f64 readF64() noexcept { return std::bit_cast<f64>(readUnsigned<u64>()); }
    [[nodiscard]] bool readBool() noexcept { return readU8() != 0; }

    [[nodiscard]] u64 readVarU64() noexcept;
    [[nodiscard]] i64 readVarI64() noexcept;
    [[nodiscard]] std::string readString();

    // A view of the next `count` bytes, without copying.
    [[nodiscard]] std::span<const std::byte> readBytes(usize count) noexcept;

    // False once any read went past the end or met invalid data.
    [[nodiscard]] bool ok() const noexcept { return !m_failed; }
    [[nodiscard]] usize position() const noexcept { return m_position; }
    [[nodiscard]] usize remaining() const noexcept { return m_bytes.size() - m_position; }
    [[nodiscard]] bool atEnd() const noexcept { return m_position == m_bytes.size(); }

private:
    template <std::unsigned_integral T>
    [[nodiscard]] T readUnsigned() noexcept {
        if (m_failed || remaining() < sizeof(T)) {
            m_failed = true;
            return 0;
        }
        T little = 0;
        std::memcpy(&little, m_bytes.data() + m_position, sizeof(T));
        m_position += sizeof(T);
        return detail::toLittleEndian(little); // Converting back is the same swap.
    }

    std::span<const std::byte> m_bytes;
    usize m_position = 0;
    bool m_failed = false;
};

} // namespace vxe::io
