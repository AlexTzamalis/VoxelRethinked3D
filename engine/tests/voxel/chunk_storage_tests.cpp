#include "terrain_sample.hpp"

#include <vxe/core/memory.hpp>
#include <vxe/voxel/chunk_storage.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <random>
#include <vector>

using namespace vxe;
using voxel::BlockStateId;
using voxel::ChunkStorage;
using Mode = ChunkStorage::Mode;

namespace {

using States = std::array<BlockStateId, kChunkVolume>;

States decompress(const ChunkStorage& storage) {
    States states{};
    storage.decompressTo(states);
    return states;
}

} // namespace

TEST_CASE("Uniform chunks cost no more than 32 bytes") {
    const ChunkStorage air;
    CHECK(air.mode() == Mode::Uniform);
    CHECK(air.get({.x = 3, .y = 4, .z = 5}) == voxel::kAirState);
    CHECK(air.memoryBytes() <= 32); // Section 2: all air or all stone chunks.
    CHECK(air.dataBytes() == 0);
    const ChunkStorage stone(1);
    CHECK(stone.get({.x = 15, .y = 15, .z = 15}) == 1);
    CHECK(stone.memoryBytes() == sizeof(ChunkStorage));
    MESSAGE("Uniform chunk: " << stone.memoryBytes() << " bytes");
}

TEST_CASE("Writes widen the palette step by step and then switch to raw") {
    ChunkStorage storage;
    States expected{};
    const auto write = [&](u16 index, BlockStateId state) {
        storage.setIndex(index, state);
        expected[index] = state;
    };
    write(0, voxel::kAirState); // Same state: stays uniform.
    CHECK(storage.isUniform());
    write(100, 1);
    CHECK(storage.mode() == Mode::Palette);
    CHECK(storage.bitsPerIndex() == 1);
    CHECK(storage.dataBytes() == 512 + 8);
    write(200, 2);
    CHECK(storage.bitsPerIndex() == 2);
    for (BlockStateId s = 3; s < 5; ++s) {
        write(static_cast<u16>(s * 10), s);
    }
    CHECK(storage.bitsPerIndex() == 4);
    for (BlockStateId s = 5; s < 17; ++s) {
        write(static_cast<u16>(s * 10), s);
    }
    CHECK(storage.bitsPerIndex() == 8);
    CHECK(storage.paletteSize() == 17);
    for (BlockStateId s = 17; s < 256; ++s) {
        write(static_cast<u16>(s * 10), s);
    }
    CHECK(storage.mode() == Mode::Palette);
    CHECK(storage.paletteSize() == 256);
    write(4000, 1000); // The 257th state.
    CHECK(storage.mode() == Mode::Raw);
    CHECK(storage.dataBytes() == 8192);
    CHECK(decompress(storage) == expected);
    for (u16 i = 0; i < kChunkVolume; ++i) {
        REQUIRE(storage.getIndex(i) == expected[i]);
    }
}

TEST_CASE("Random writes always read back, in every mode") {
    std::mt19937 random(12345); // NOLINT(bugprone-random-generator-seed): the same data every run.
    for (const u32 distinct : {2U, 4U, 9U, 40U, 300U}) {
        ChunkStorage storage;
        States expected{};
        std::uniform_int_distribution<u32> index(0, kChunkVolume - 1);
        std::uniform_int_distribution<u32> state(0, distinct - 1);
        for (int i = 0; i < 20000; ++i) {
            const auto at = static_cast<u16>(index(random));
            const auto value = static_cast<BlockStateId>(state(random) * 7);
            storage.setIndex(at, value);
            expected[at] = value;
        }
        CHECK(decompress(storage) == expected);
        const LocalPos probe{.x = 7, .y = 3, .z = 11};
        CHECK(storage.get(probe) == expected[probe.index()]);
        storage.compact();
        CHECK(decompress(storage) == expected);
        MESSAGE(distinct << " states: mode " << static_cast<int>(storage.mode()) << ", "
                         << storage.bitsPerIndex() << " bits, " << storage.dataBytes() << " bytes");
    }
}

TEST_CASE("Compaction drops unused states and shrinks the storage") {
    ChunkStorage storage;
    for (u16 i = 0; i < 5; ++i) {
        storage.setIndex(i, static_cast<BlockStateId>(i + 1));
    }
    CHECK(storage.bitsPerIndex() == 4); // Air plus 5 states.
    for (u16 i = 1; i < 5; ++i) {
        storage.setIndex(i, voxel::kAirState); // Only state 1 at index 0 is left.
    }
    CHECK(storage.paletteSize() == 6); // Unused entries stay until compaction.
    storage.compact();
    CHECK(storage.bitsPerIndex() == 1);
    CHECK(storage.paletteSize() == 2);
    CHECK(storage.get(LocalPos::fromIndex(0)) == 1);
    storage.setIndex(0, voxel::kAirState);
    storage.compact();
    CHECK(storage.isUniform());
    CHECK(storage.uniformState() == voxel::kAirState);

    // Raw goes back to a palette once few states are left.
    ChunkStorage raw;
    for (u16 i = 0; i < 300; ++i) {
        raw.setIndex(i, static_cast<BlockStateId>(i + 1));
    }
    REQUIRE(raw.mode() == Mode::Raw);
    raw.compact();
    CHECK(raw.mode() == Mode::Raw); // 301 states still need raw.
    for (u16 i = 3; i < 300; ++i) {
        raw.setIndex(i, 2);
    }
    raw.compact();
    CHECK(raw.mode() == Mode::Palette);
    CHECK(raw.bitsPerIndex() == 2); // Air, 1, 2, 3.
    CHECK(raw.get(LocalPos::fromIndex(2)) == 3);
    CHECK(raw.get(LocalPos::fromIndex(299)) == 2);
}

TEST_CASE("Assign picks the smallest mode for the data") {
    States states{};
    ChunkStorage storage;
    states.fill(1);
    storage.assign(states);
    CHECK(storage.isUniform());
    CHECK(storage.uniformState() == 1);
    states[17] = 2;
    states[4095] = 3;
    storage.assign(states);
    CHECK(storage.mode() == Mode::Palette);
    CHECK(storage.bitsPerIndex() == 2);
    CHECK(decompress(storage) == states);
}

TEST_CASE("Copies are deep, moves leave air, fill frees the memory") {
    const memory::TagStats before = memory::stats(MemoryTag::Chunks);
    {
        ChunkStorage original;
        original.set({.x = 1, .y = 2, .z = 3}, 5);
        ChunkStorage copy = original;
        copy.set({.x = 1, .y = 2, .z = 3}, 6);
        CHECK(original.get({.x = 1, .y = 2, .z = 3}) == 5);
        CHECK(copy.get({.x = 1, .y = 2, .z = 3}) == 6);
        ChunkStorage moved = std::move(original);
        CHECK(moved.get({.x = 1, .y = 2, .z = 3}) == 5);
        CHECK(original.isUniform()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move):
                                     // documented state.
        // The copy got a third state and widened to 2 bits; the move kept the 1 bit block.
        CHECK(copy.bitsPerIndex() == 2);
        CHECK(memory::stats(MemoryTag::Chunks).currentBytes - before.currentBytes ==
              moved.dataBytes() + copy.dataBytes());
        moved.fill(9);
        CHECK(moved.dataBytes() == 0);
        copy = moved;
        CHECK(copy.isUniform());
    }
    CHECK(memory::stats(MemoryTag::Chunks).currentBytes == before.currentBytes);
}

// Section 2 targets: all air or all stone chunks at most 32 bytes, mixed
// chunks at most 2 KB of block data on average, measured on the synthetic
// terrain sample (8 x 8 chunk columns, 12 chunks tall).
TEST_CASE("The terrain sample meets the Section 2 memory targets") {
    States states{};
    usize uniformChunks = 0;
    usize mixedChunks = 0;
    usize mixedBytes = 0;
    usize largestUniform = 0;
    std::array<usize, 9> byBits{};
    for (i32 cz = 0; cz < 8; ++cz) {
        for (i32 cx = 0; cx < 8; ++cx) {
            for (i32 cy = -4; cy < 8; ++cy) {
                test::sampleChunk({.x = cx, .y = cy, .z = cz}, states);
                ChunkStorage storage;
                storage.assign(states);
                REQUIRE(decompress(storage) == states);
                if (storage.isUniform()) {
                    ++uniformChunks;
                    largestUniform = std::max(largestUniform, storage.memoryBytes());
                } else {
                    ++mixedChunks;
                    mixedBytes += storage.dataBytes();
                    ++byBits.at(storage.bitsPerIndex());
                }
            }
        }
    }
    const f64 averageMixed = static_cast<f64>(mixedBytes) / static_cast<f64>(mixedChunks);
    MESSAGE(uniformChunks << " uniform chunks (" << largestUniform << " bytes each), " << mixedChunks
                          << " mixed chunks averaging " << averageMixed
                          << " bytes of block data; indices: " << byBits[1] << " x 1 bit, " << byBits[2]
                          << " x 2, " << byBits[4] << " x 4, " << byBits[8] << " x 8");
    CHECK(largestUniform <= 32);
    CHECK(averageMixed <= 2048.0);
    CHECK(mixedChunks > 0);
    CHECK(uniformChunks > 0);
}

TEST_CASE("Strided decompression writes every mode into a larger box") {
    std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed): the same data every run.
    std::vector<ChunkStorage> storages;
    storages.emplace_back(7); // Uniform.
    for (const u32 distinct : {2U, 4U, 16U, 200U, 400U}) {
        ChunkStorage storage;
        for (u16 i = 0; i < kChunkVolume; ++i) {
            storage.setIndex(i, static_cast<BlockStateId>(random() % distinct));
        }
        storages.push_back(std::move(storage));
    }
    constexpr usize kRow = 19; // Odd strides catch mixed up axes.
    constexpr usize kSlice = 21 * kRow;
    for (const ChunkStorage& storage : storages) {
        std::vector<BlockStateId> box((16 * kSlice) + 5, 0xFFFF);
        storage.decompressTo(box, kRow, kSlice);
        const States flat = decompress(storage);
        for (u16 i = 0; i < kChunkVolume; ++i) {
            const LocalPos p = LocalPos::fromIndex(i);
            REQUIRE(box[(p.y * kSlice) + (p.z * kRow) + p.x] == flat[i]);
        }
        CHECK(box[16] == 0xFFFF); // Gaps between rows stay untouched.
        CHECK(box.back() == 0xFFFF);
    }
}
