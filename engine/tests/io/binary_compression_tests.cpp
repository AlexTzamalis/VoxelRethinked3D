#include <vxe/io/binary.hpp>
#include <vxe/io/compression.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using vxe::io::BinaryReader;
using vxe::io::BinaryWriter;
using vxe::io::Codec;

namespace {

std::vector<std::byte> compressibleData(std::size_t size) {
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i / 64) % 7); // Long runs, like an all stone chunk.
    }
    return data;
}

std::vector<std::byte> randomData(std::size_t size) {
    std::mt19937 rng(12345); // NOLINT(bugprone-random-generator-seed): tests need the same data every run.
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<std::byte> data(size);
    for (std::byte& b : data) {
        b = static_cast<std::byte>(byte(rng));
    }
    return data;
}

} // namespace

TEST_CASE("BinaryWriter writes little endian on every platform") {
    BinaryWriter writer;
    writer.writeU32(0x11223344U);
    writer.writeU16(0xAABBU);
    const auto bytes = writer.bytes();
    REQUIRE(bytes.size() == 6);
    CHECK(bytes[0] == std::byte{0x44});
    CHECK(bytes[1] == std::byte{0x33});
    CHECK(bytes[2] == std::byte{0x22});
    CHECK(bytes[3] == std::byte{0x11});
    CHECK(bytes[4] == std::byte{0xBB});
    CHECK(bytes[5] == std::byte{0xAA});
}

TEST_CASE("Every value type round trips") {
    BinaryWriter writer;
    writer.writeU8(200);
    writer.writeI8(-100);
    writer.writeU16(65'000);
    writer.writeI16(-32'000);
    writer.writeU32(4'000'000'000U);
    writer.writeI32(std::numeric_limits<vxe::i32>::min());
    writer.writeU64(std::numeric_limits<vxe::u64>::max());
    writer.writeI64(-1);
    writer.writeF32(3.5F);
    writer.writeF64(-1.0 / 3.0);
    writer.writeF32(std::numeric_limits<float>::quiet_NaN());
    writer.writeBool(true);
    writer.writeString("Voxel Rethinked");
    writer.writeString("");

    BinaryReader reader(writer.bytes());
    CHECK(reader.readU8() == 200);
    CHECK(reader.readI8() == -100);
    CHECK(reader.readU16() == 65'000);
    CHECK(reader.readI16() == -32'000);
    CHECK(reader.readU32() == 4'000'000'000U);
    CHECK(reader.readI32() == std::numeric_limits<vxe::i32>::min());
    CHECK(reader.readU64() == std::numeric_limits<vxe::u64>::max());
    CHECK(reader.readI64() == -1);
    CHECK(reader.readF32() == 3.5F);
    CHECK(reader.readF64() == -1.0 / 3.0);
    CHECK(std::isnan(reader.readF32()));
    CHECK(reader.readBool());
    CHECK(reader.readString() == "Voxel Rethinked");
    CHECK(reader.readString().empty());
    CHECK(reader.ok());
    CHECK(reader.atEnd());
}

TEST_CASE("Varints are short for small values and exact at the extremes") {
    BinaryWriter writer;
    writer.writeVarU64(0);
    writer.writeVarU64(127);
    CHECK(writer.size() == 2); // One byte each.
    writer.writeVarU64(128);
    CHECK(writer.size() == 4); // 128 needs two.

    constexpr std::array<vxe::u64, 5> kUnsigned{300, 16'384, 1ULL << 35U,
                                                std::numeric_limits<vxe::u64>::max(), 1};
    constexpr std::array<vxe::i64, 6> kSigned{
        0, -1, 1, -64, std::numeric_limits<vxe::i64>::min(), std::numeric_limits<vxe::i64>::max()};
    for (const vxe::u64 value : kUnsigned) {
        writer.writeVarU64(value);
    }
    for (const vxe::i64 value : kSigned) {
        writer.writeVarI64(value);
    }

    BinaryReader reader(writer.bytes());
    CHECK(reader.readVarU64() == 0);
    CHECK(reader.readVarU64() == 127);
    CHECK(reader.readVarU64() == 128);
    for (const vxe::u64 value : kUnsigned) {
        CHECK(reader.readVarU64() == value);
    }
    for (const vxe::i64 value : kSigned) {
        CHECK(reader.readVarI64() == value);
    }
    CHECK(reader.ok());
    CHECK(reader.atEnd());
}

TEST_CASE("Reading past the end fails safely and stays failed") {
    BinaryWriter writer;
    writer.writeU16(7);
    BinaryReader reader(writer.bytes());
    CHECK(reader.readU32() == 0); // Only 2 bytes available.
    CHECK_FALSE(reader.ok());
    CHECK(reader.readU8() == 0); // Sticky: even a fitting read now fails.
    CHECK(reader.readString().empty());
    CHECK(reader.readBytes(1).empty());
}

TEST_CASE("Malformed varints and string lengths are rejected") {
    // Eleven continuation bytes: longer than any valid u64.
    std::vector<std::byte> tooLong(11, std::byte{0xFF});
    BinaryReader longReader(tooLong);
    CHECK(longReader.readVarU64() == 0);
    CHECK_FALSE(longReader.ok());

    // A string that claims a million bytes in a 3 byte buffer.
    BinaryWriter writer;
    writer.writeVarU64(1'000'000);
    BinaryReader stringReader(writer.bytes());
    CHECK(stringReader.readString().empty());
    CHECK_FALSE(stringReader.ok());
}

TEST_CASE("zstd and LZ4 round trip compressible and random data") {
    for (const Codec codec : {Codec::Zstd, Codec::Lz4, Codec::None}) {
        CAPTURE(static_cast<int>(codec));
        for (const auto& data :
             {compressibleData(std::size_t{64} * 1024), randomData(10'000), std::vector<std::byte>{}}) {
            const auto compressed = vxe::io::compress(data, codec);
            REQUIRE(compressed.hasValue());
            const auto restored = vxe::io::decompress(compressed.value(), codec, data.size());
            REQUIRE(restored.hasValue());
            CHECK(restored.value() == data);
        }
    }
}

TEST_CASE("Compression actually shrinks repetitive data") {
    const auto data = compressibleData(std::size_t{64} * 1024);
    CHECK(vxe::io::compress(data, Codec::Zstd).value().size() < data.size() / 20);
    CHECK(vxe::io::compress(data, Codec::Lz4).value().size() < data.size() / 10);
    // Higher zstd levels never produce larger output for this kind of data.
    CHECK(vxe::io::compress(data, Codec::Zstd, 19).value().size() <=
          vxe::io::compress(data, Codec::Zstd, 1).value().size());
}

TEST_CASE("Corrupt data and wrong sizes are reported, never overrun") {
    const auto data = compressibleData(4096);
    for (const Codec codec : {Codec::Zstd, Codec::Lz4}) {
        CAPTURE(static_cast<int>(codec));
        auto compressed = vxe::io::compress(data, codec).value();
        CHECK(vxe::io::decompress(compressed, codec, data.size() - 1).hasError());   // Too small a buffer.
        CHECK(vxe::io::decompress(compressed, codec, data.size() + 100).hasError()); // Too large.

        for (std::size_t i = 0; i < compressed.size(); i += 3) {
            compressed[i] = ~compressed[i];
        }
        const auto damaged = vxe::io::decompress(compressed, codec, data.size());
        // Damage is either detected or (rarely, for LZ4 without a checksum)
        // decodes to different bytes; it never crashes or overruns.
        CHECK((damaged.hasError() || damaged.value() != data));
    }
}

TEST_CASE("compressInto reuses a caller buffer") {
    const auto data = compressibleData(8192);
    std::vector<std::byte> buffer(vxe::io::maxCompressedSize(data.size(), Codec::Zstd));
    const auto written = vxe::io::compressInto(data, buffer, Codec::Zstd);
    REQUIRE(written.hasValue());
    std::vector<std::byte> restored(data.size());
    REQUIRE(
        vxe::io::decompressInto(std::span(buffer).first(written.value()), restored, Codec::Zstd).hasValue());
    CHECK(restored == data);

    std::array<std::byte, 4> tiny{};
    CHECK(vxe::io::compressInto(data, tiny, Codec::Lz4).hasError());
}
