#include <vxe/core/containers.hpp>
#include <vxe/math/coords.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <format>
#include <limits>

using vxe::BlockPos;
using vxe::ChunkPos;
using vxe::i32;
using vxe::i64;
using vxe::LocalPos;

TEST_CASE("Positive blocks map to their chunk and local position") {
    constexpr BlockPos kBlock{.x = 35, .y = 0, .z = 15};
    static_assert(vxe::toChunk(kBlock) == ChunkPos{.x = 2, .y = 0, .z = 0});
    static_assert(vxe::toLocal(kBlock) == LocalPos{.x = 3, .y = 0, .z = 15});
    CHECK(vxe::toBlock(vxe::toChunk(kBlock), vxe::toLocal(kBlock)) == kBlock);
}

TEST_CASE("Negative blocks round towards negative infinity") {
    // Block -1 is the last block of chunk -1, not part of chunk 0.
    static_assert(vxe::toChunk(BlockPos{.x = -1, .y = -1, .z = -1}) == ChunkPos{.x = -1, .y = -1, .z = -1});
    static_assert(vxe::toLocal(BlockPos{.x = -1, .y = -1, .z = -1}) == LocalPos{.x = 15, .y = 15, .z = 15});
    static_assert(vxe::toChunk(BlockPos{.x = -16, .y = -17, .z = -32}) ==
                  ChunkPos{.x = -1, .y = -2, .z = -2});
    static_assert(vxe::toLocal(BlockPos{.x = -16, .y = -17, .z = -32}) == LocalPos{.x = 0, .y = 15, .z = 0});

    for (i64 x = -100; x <= 100; ++x) {
        const BlockPos block{.x = x, .y = -x, .z = x * 7};
        CHECK(vxe::toBlock(vxe::toChunk(block), vxe::toLocal(block)) == block);
    }
}

TEST_CASE("Chunk grid extremes do not overflow") {
    constexpr i32 kMin = std::numeric_limits<i32>::min();
    constexpr i32 kMax = std::numeric_limits<i32>::max();

    constexpr ChunkPos kLowest{.x = kMin, .y = kMin, .z = kMin};
    constexpr ChunkPos kHighest{.x = kMax, .y = kMax, .z = kMax};
    constexpr BlockPos kLowestBlock = vxe::chunkOrigin(kLowest);
    constexpr BlockPos kHighestBlock = vxe::toBlock(kHighest, LocalPos{.x = 15, .y = 15, .z = 15});

    static_assert(kLowestBlock.x == static_cast<i64>(kMin) * 16);
    static_assert(kHighestBlock.x == (static_cast<i64>(kMax) * 16) + 15);
    static_assert(vxe::toChunk(kLowestBlock) == kLowest);
    static_assert(vxe::toChunk(kHighestBlock) == kHighest);
    static_assert(vxe::chebyshevDistance(kLowest, kHighest) == static_cast<i64>(kMax) - kMin);
}

TEST_CASE("Local index is Y major and round trips") {
    static_assert(LocalPos{.x = 1, .y = 0, .z = 0}.index() == 1);
    static_assert(LocalPos{.x = 0, .y = 0, .z = 1}.index() == 16);
    static_assert(LocalPos{.x = 0, .y = 1, .z = 0}.index() == 256);
    static_assert(LocalPos{.x = 15, .y = 15, .z = 15}.index() == 4095);

    for (vxe::u16 i = 0; i < vxe::kChunkVolume; ++i) {
        REQUIRE(LocalPos::fromIndex(i).index() == i);
    }
}

TEST_CASE("blockAt rounds entity positions down and clamps garbage") {
    CHECK(vxe::blockAt({0.5, 64.99, -0.5}) == BlockPos{.x = 0, .y = 64, .z = -1});
    CHECK(vxe::blockAt({-16.0, -0.0001, 30'000'000.25}) == BlockPos{.x = -16, .y = -1, .z = 30'000'000});
    CHECK(vxe::blockAt({1e300, -1e300, std::numeric_limits<double>::quiet_NaN()}) ==
          BlockPos{.x = std::numeric_limits<i64>::max(), .y = std::numeric_limits<i64>::min(), .z = 0});
}

TEST_CASE("Chunk offsets and distances") {
    constexpr ChunkPos kOrigin{};
    static_assert(kOrigin.offset(1, -2, 3) == ChunkPos{.x = 1, .y = -2, .z = 3});
    CHECK(kOrigin + vxe::IVec3(4, 5, 6) == ChunkPos{.x = 4, .y = 5, .z = 6});
    CHECK((ChunkPos{.x = 3, .y = 0, .z = 0} - ChunkPos{.x = 1, .y = 2, .z = 0}) == vxe::IVec3(2, -2, 0));
    static_assert(vxe::chebyshevDistance(kOrigin, ChunkPos{.x = -5, .y = 2, .z = 3}) == 5);
}

TEST_CASE("ChunkPos hashes spread neighbors and key a HashMap") {
    vxe::HashSet<std::size_t> hashes;
    for (i32 x = -8; x < 8; ++x) {
        for (i32 y = -8; y < 8; ++y) {
            for (i32 z = -8; z < 8; ++z) {
                hashes.insert(std::hash<ChunkPos>{}(ChunkPos{.x = x, .y = y, .z = z}));
            }
        }
    }
    CHECK(hashes.size() == 16 * 16 * 16); // No collisions among 4096 neighbors.

    vxe::HashMap<ChunkPos, int> chunks;
    chunks[ChunkPos{.x = -1, .y = 0, .z = 1}] = 7;
    CHECK(chunks.at(ChunkPos{.x = -1, .y = 0, .z = 1}) == 7);
}

TEST_CASE("Positions format for logs") {
    CHECK(std::format("{}", ChunkPos{.x = -1, .y = 2, .z = 3}) == "chunk(-1, 2, 3)");
    CHECK(std::format("{}", BlockPos{.x = 10, .y = -20, .z = 30}) == "block(10, -20, 30)");
    CHECK(std::format("{}", LocalPos{.x = 1, .y = 2, .z = 3}) == "local(1, 2, 3)");
}
