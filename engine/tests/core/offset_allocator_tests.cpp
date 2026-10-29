#include <vxe/core/offset_allocator.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <vector>

using namespace vxe;

TEST_CASE("Size bins round up and down around exact small floats") {
    for (u32 size = 0; size < 8; ++size) {
        CHECK(OffsetAllocator::binRoundUp(size) == size); // Exact below 8.
        CHECK(OffsetAllocator::binSize(OffsetAllocator::binRoundDown(size)) == size);
    }
    std::mt19937 random(3); // NOLINT(bugprone-random-generator-seed): the same sizes every run.
    for (int i = 0; i < 10000; ++i) {
        // Up to 2^31: the largest bins hold sizes beyond 32 bits.
        const u32 size = 1 + (static_cast<u32>(random()) >> static_cast<u32>(1 + (random() % 31)));
        const u64 up = OffsetAllocator::binSize(OffsetAllocator::binRoundUp(size));
        const u64 down = OffsetAllocator::binSize(OffsetAllocator::binRoundDown(size));
        REQUIRE(up >= size);
        REQUIRE(down <= size);
        // Bins are at most an eighth apart (3 mantissa bits).
        REQUIRE((up - down) * 8 <= down);
    }
    CHECK(OffsetAllocator::binRoundUp(0xFFFFFFFFU) < 256);
}

TEST_CASE("Allocations do not overlap and freeing merges the space back") {
    OffsetAllocator allocator(1000, 16);
    const auto a = allocator.allocate(100);
    const auto b = allocator.allocate(200);
    const auto c = allocator.allocate(300);
    REQUIRE(a.isValid());
    REQUIRE(b.isValid());
    REQUIRE(c.isValid());
    CHECK(a.offset == 0);
    CHECK(b.offset == 100);
    CHECK(c.offset == 300);
    CHECK(allocator.sizeOf(b) == 200);
    CHECK(allocator.freeSize() == 400);
    CHECK_FALSE(allocator.allocate(500).isValid()); // Not enough left.
    allocator.free(b);
    const auto d = allocator.allocate(150); // Fits into b's old place.
    CHECK(d.offset == 100);
    allocator.free(a);
    allocator.free(c);
    allocator.free(d);
    CHECK(allocator.freeSize() == 1000);
    CHECK(allocator.report().allocations == 0);
    const auto whole = allocator.allocate(1000); // Everything merged into one range again.
    CHECK(whole.offset == 0);
    allocator.free(whole);
    allocator.free({}); // Invalid allocations are ignored.
}

TEST_CASE("The allocation limit is enforced and reset frees everything") {
    OffsetAllocator allocator(100, 4);
    std::vector<OffsetAllocator::Allocation> live;
    for (int i = 0; i < 4; ++i) {
        live.push_back(allocator.allocate(10));
        REQUIRE(live.back().isValid());
    }
    CHECK_FALSE(allocator.allocate(10).isValid());
    allocator.free(live[1]);
    CHECK(allocator.allocate(5).isValid());
    allocator.reset();
    CHECK(allocator.freeSize() == 100);
    CHECK(allocator.allocate(100).offset == 0);
}

// Random allocations and frees, checked against a byte map of the space:
// no two live allocations overlap, the free total always matches, and after
// freeing everything the whole space is one range again.
TEST_CASE("Random allocation and free never overlaps") {
    constexpr u32 kSize = 1U << 16U;
    OffsetAllocator allocator(kSize, 2048);
    std::vector<u8> owner(kSize, 0);
    struct Live {
        OffsetAllocator::Allocation allocation;
        u32 size;
    };
    std::vector<Live> live;
    std::mt19937 random(77); // NOLINT(bugprone-random-generator-seed): the same sequence every run.
    u32 used = 0;
    usize failures = 0;
    for (int step = 0; step < 50000; ++step) {
        if (live.empty() || random() % 100 < 55) {
            const u32 size = 1 + (random() % (random() % 4 == 0 ? 2000U : 60U));
            const auto allocation = allocator.allocate(size);
            if (!allocation.isValid()) {
                ++failures;
                continue;
            }
            REQUIRE(allocation.offset + size <= kSize);
            for (u32 i = 0; i < size; ++i) {
                REQUIRE(owner[allocation.offset + i] == 0);
                owner[allocation.offset + i] = 1;
            }
            live.push_back({.allocation = allocation, .size = size});
            used += size;
        } else {
            const usize index = random() % live.size();
            const Live entry = live[index];
            live[index] = live.back();
            live.pop_back();
            REQUIRE(allocator.sizeOf(entry.allocation) == entry.size);
            allocator.free(entry.allocation);
            std::fill_n(owner.begin() + entry.allocation.offset, entry.size, u8{0});
            used -= entry.size;
        }
        REQUIRE(allocator.freeSize() == kSize - used);
    }
    MESSAGE(live.size() << " live at the end, " << failures << " allocations did not fit");
    for (const Live& entry : live) {
        allocator.free(entry.allocation);
    }
    CHECK(allocator.freeSize() == kSize);
    CHECK(allocator.allocate(kSize).offset == 0);
}
