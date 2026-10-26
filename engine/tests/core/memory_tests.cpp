#include <vxe/core/linear_allocator.hpp>
#include <vxe/core/memory.hpp>
#include <vxe/core/pool_allocator.hpp>
#include <vxe/core/scratch.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <set>
#include <thread>
#include <vector>

using vxe::MemoryTag;
using vxe::usize;

namespace {

bool isAligned(const void* pointer, usize alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

// Counts constructions and destructions to prove the pool runs both.
struct Tracked {
    static inline int alive = 0;
    int value;
    explicit Tracked(int v)
        : value(v) {
        ++alive;
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked(Tracked&&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() { --alive; }
};

} // namespace

TEST_CASE("Tag stats follow allocate and deallocate") {
    // The Debug tag is otherwise unused by the tests, so its counters are ours.
    const auto before = vxe::memory::stats(MemoryTag::Debug);

    void* block = vxe::memory::allocate(1000, 64, MemoryTag::Debug);
    CHECK(isAligned(block, 64));
    const auto during = vxe::memory::stats(MemoryTag::Debug);
    CHECK(during.currentBytes == before.currentBytes + 1000);
    CHECK(during.allocations == before.allocations + 1);
    CHECK(during.peakBytes >= during.currentBytes);
    CHECK(vxe::memory::totalBytes() >= 1000);

    vxe::memory::deallocate(block, 1000, 64, MemoryTag::Debug);
    const auto after = vxe::memory::stats(MemoryTag::Debug);
    CHECK(after.currentBytes == before.currentBytes);
    CHECK(after.frees == before.frees + 1);
    CHECK(after.peakBytes >= before.currentBytes + 1000);

    CHECK(vxe::memory::nameOf(MemoryTag::Chunks) == "Chunks");
}

TEST_CASE("TrackingAllocator records container memory under its tag") {
    const usize before = vxe::memory::stats(MemoryTag::Ui).currentBytes;
    {
        std::vector<int, vxe::TrackingAllocator<int, MemoryTag::Ui>> values;
        values.reserve(256);
        values.push_back(1);
        CHECK(vxe::memory::stats(MemoryTag::Ui).currentBytes >= before + (256 * sizeof(int)));
    }
    CHECK(vxe::memory::stats(MemoryTag::Ui).currentBytes == before);
}

TEST_CASE("LinearAllocator aligns, fills up and rewinds") {
    vxe::LinearAllocator arena(1024, MemoryTag::General);
    CHECK(arena.capacityBytes() == 1024);

    const void* a = arena.allocate(3, 1);
    const void* b = arena.allocate(16, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(isAligned(b, 16));
    CHECK(arena.usedBytes() >= 19);

    const auto marker = arena.marker();
    CHECK(arena.allocate(512, 8) != nullptr);
    arena.rewind(marker);
    CHECK(arena.usedBytes() == marker);

    // A request larger than what is left fails without changing the arena.
    const usize used = arena.usedBytes();
    CHECK(arena.allocate(4096, 8) == nullptr);
    CHECK(arena.usedBytes() == used);

    auto values = arena.allocateArray<vxe::u32>(10);
    REQUIRE(values.size() == 10);
    CHECK(isAligned(values.data(), alignof(vxe::u32)));
    CHECK(arena.allocateArray<vxe::u64>(10'000).empty());

    arena.reset();
    CHECK(arena.usedBytes() == 0);
    CHECK(arena.peakBytes() >= 512);
}

TEST_CASE("LinearAllocator supports alignments above its block alignment") {
    vxe::LinearAllocator arena(8192);
    static_cast<void>(arena.allocate(1, 1));
    const void* page = arena.allocate(64, 4096);
    REQUIRE(page != nullptr);
    CHECK(isAligned(page, 4096));
}

TEST_CASE("LinearAllocator move transfers the block") {
    vxe::LinearAllocator first(256);
    static_cast<void>(first.allocate(10, 1));
    const vxe::LinearAllocator second(std::move(first));
    CHECK(second.capacityBytes() == 256);
    CHECK(second.usedBytes() == 10);
}

TEST_CASE("PoolAllocator creates, destroys and reuses slots") {
    vxe::PoolAllocator<Tracked> pool(4);
    std::vector<Tracked*> objects;
    objects.reserve(10);
    for (int i = 0; i < 10; ++i) {
        objects.push_back(pool.create(i));
    }
    CHECK(Tracked::alive == 10);
    CHECK(pool.liveCount() == 10);
    CHECK(pool.pageCount() == 3); // 4 + 4 + 2 of 4
    CHECK(pool.capacity() == 12);

    // Pointers stay valid and distinct while the pool grows.
    const std::set<Tracked*> unique(objects.begin(), objects.end());
    CHECK(unique.size() == 10);
    for (int i = 0; i < 10; ++i) {
        CHECK(objects[static_cast<usize>(i)]->value == i);
    }

    Tracked* freed = objects[3];
    pool.destroy(freed);
    CHECK(Tracked::alive == 9);
    // The most recently freed slot is handed out first.
    Tracked* reused = pool.create(99);
    CHECK(reused == freed);
    objects[3] = reused;

    for (Tracked* object : objects) {
        pool.destroy(object);
    }
    CHECK(Tracked::alive == 0);
    CHECK(pool.liveCount() == 0);
    CHECK(pool.pageCount() == 3); // Pages are kept for reuse.
}

TEST_CASE("ScratchScope rewinds when it ends, including nested scopes") {
    const usize start = vxe::ScratchScope::threadArena().usedBytes();
    {
        vxe::ScratchScope outer;
        constexpr usize kPaddedChunkVolume = usize{18} * 18 * 18;
        auto first = outer.allocateArray<vxe::u16>(kPaddedChunkVolume);
        REQUIRE(first.size() == kPaddedChunkVolume);
        const usize afterOuter = vxe::ScratchScope::threadArena().usedBytes();
        {
            vxe::ScratchScope inner;
            CHECK(inner.allocate(1000) != nullptr);
            CHECK(vxe::ScratchScope::threadArena().usedBytes() > afterOuter);
        }
        CHECK(vxe::ScratchScope::threadArena().usedBytes() == afterOuter);
    }
    CHECK(vxe::ScratchScope::threadArena().usedBytes() == start);
}

TEST_CASE("Each thread has its own scratch arena") {
    const vxe::LinearAllocator* mainArena = &vxe::ScratchScope::threadArena();
    const vxe::LinearAllocator* workerArena = nullptr;
    std::thread worker([&workerArena] {
        const vxe::ScratchScope scratch;
        workerArena = &vxe::ScratchScope::threadArena();
    });
    worker.join();
    CHECK(workerArena != nullptr);
    CHECK(workerArena != mainArena);
}
