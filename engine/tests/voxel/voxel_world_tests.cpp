#include "../test_helpers.hpp"
#include "terrain_sample.hpp"

#include <vxe/core/memory.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <vector>

using namespace vxe;
using voxel::BlockStateId;
using voxel::Chunk;
using voxel::ChunkSnapshot;
using voxel::ChunkStorage;
using voxel::ColumnSummary;
using voxel::VoxelWorld;

namespace {

using States = std::array<BlockStateId, kChunkVolume>;

ChunkStorage sampleStorage(ChunkPos position) {
    States states{};
    test::sampleChunk(position, states);
    ChunkStorage storage;
    storage.assign(states);
    return storage;
}

// A chunk with a different state in every block column and layer, so every
// border position of a snapshot has a value that tells where it came from.
ChunkStorage patternStorage(u16 seed) {
    ChunkStorage storage;
    for (u16 i = 0; i < kChunkVolume; ++i) {
        storage.setIndex(i, static_cast<BlockStateId>(100 + ((i * 7U + seed * 31U) % 200U)));
    }
    return storage;
}

// The highest non air loaded block of a block column, found the slow way.
i32 bruteForceHighest(const VoxelWorld& world, i64 x, i64 z, i64 minY, i64 maxY) {
    for (i64 y = maxY; y >= minY; --y) {
        if (world.getBlock({.x = x, .y = y, .z = z}) != voxel::kAirState) {
            return static_cast<i32>(y);
        }
    }
    return ColumnSummary::kNoBlock;
}

} // namespace

TEST_CASE("Chunks are inserted, found and removed") {
    VoxelWorld world;
    CHECK(world.chunkCount() == 0);
    CHECK(world.findChunk({.x = 0, .y = 0, .z = 0}) == nullptr);
    CHECK(world.getBlock({.x = 5, .y = 5, .z = 5}) == voxel::kAirState);
    CHECK(world.getBlock({.x = 5, .y = 5, .z = 5}, 77) == 77);
    CHECK_FALSE(world.setBlock({.x = 5, .y = 5, .z = 5}, 1)); // Not loaded.

    Chunk& stone = world.insertChunk({.x = -1, .y = -2, .z = 3}, ChunkStorage(1));
    CHECK(stone.position() == ChunkPos{.x = -1, .y = -2, .z = 3});
    CHECK(world.findChunk({.x = -1, .y = -2, .z = 3}) == &stone);
    CHECK(world.getBlock({.x = -16, .y = -32, .z = 48}) == 1);
    CHECK(world.getBlock({.x = -1, .y = -17, .z = 63}) == 1);
    CHECK(world.getBlock({.x = 0, .y = -17, .z = 63}) == voxel::kAirState); // Next chunk.

    CHECK(world.setBlock({.x = -3, .y = -20, .z = 50}, 4));
    CHECK(world.getBlock({.x = -3, .y = -20, .z = 50}) == 4);
    CHECK(world.chunkCount() == 1);
    CHECK(world.removeChunk({.x = -1, .y = -2, .z = 3}));
    CHECK_FALSE(world.removeChunk({.x = -1, .y = -2, .z = 3}));
    CHECK(world.chunkCount() == 0);
    CHECK(world.columnCount() == 0);
    CHECK(memory::stats(MemoryTag::Chunks).currentBytes > 0); // The pools keep their pages.
}

TEST_CASE("Inserting a loaded position replaces its blocks in place") {
    VoxelWorld world;
    Chunk& first = world.insertChunk({.x = 0, .y = 0, .z = 0});
    const u64 version = first.version();
    Chunk& second = world.insertChunk({.x = 0, .y = 0, .z = 0}, ChunkStorage(2));
    CHECK(&first == &second);
    CHECK(world.chunkCount() == 1);
    CHECK(second.blocks().uniformState() == 2);
    CHECK(second.version() != version);
}

TEST_CASE("Versions change for every edit a snapshot of the chunk would see") {
    VoxelWorld world;
    for (i32 y = -1; y <= 1; ++y) {
        for (i32 z = -1; z <= 1; ++z) {
            for (i32 x = -1; x <= 1; ++x) {
                world.insertChunk({.x = x, .y = y, .z = z});
            }
        }
    }
    const auto versions = [&] {
        std::vector<u64> result;
        world.forEachChunk([&](const Chunk& chunk) { result.push_back(chunk.version()); });
        return result;
    };
    const auto changedChunks = [&](const std::vector<u64>& before) {
        const std::vector<u64> after = versions();
        usize changed = 0;
        for (usize i = 0; i < before.size(); ++i) {
            changed += before[i] != after[i] ? 1U : 0U;
        }
        return changed;
    };

    std::vector<u64> before = versions();
    world.setBlock({.x = 7, .y = 7, .z = 7}, 1); // Inside: only the chunk itself.
    CHECK(changedChunks(before) == 1);
    before = versions();
    world.setBlock({.x = 7, .y = 7, .z = 7}, 1); // Same state: nothing.
    CHECK(changedChunks(before) == 0);
    before = versions();
    world.setBlock({.x = 15, .y = 7, .z = 7}, 1); // On a face: the chunk and one neighbor.
    CHECK(changedChunks(before) == 2);
    before = versions();
    world.setBlock({.x = 0, .y = 15, .z = 7}, 1); // On an edge: 1 + 3.
    CHECK(changedChunks(before) == 4);
    before = versions();
    world.setBlock({.x = 0, .y = 0, .z = 0}, 1); // On a corner: 1 + 7.
    CHECK(changedChunks(before) == 8);

    // Loading and unloading a chunk changes all 26 neighbors' snapshots.
    const u64 centerBefore = world.findChunk({})->version();
    world.removeChunk({.x = 1, .y = 1, .z = 1});
    const u64 afterRemove = world.findChunk({})->version();
    CHECK(afterRemove != centerBefore);
    // A reloaded chunk never repeats a version an old job could hold.
    const Chunk& reloaded = world.insertChunk({.x = 1, .y = 1, .z = 1});
    CHECK(reloaded.version() > afterRemove);
    CHECK(world.findChunk({})->version() == reloaded.version());
}

TEST_CASE("Dirty flags: loading needs a mesh, edits also need a save") {
    VoxelWorld world;
    Chunk& chunk = world.insertChunk({}, ChunkStorage(1));
    CHECK(chunk.isDirty(voxel::kDirtyMesh));
    CHECK_FALSE(chunk.isDirty(voxel::kDirtySave));
    chunk.clearDirty(voxel::kDirtyMesh);
    CHECK(chunk.dirtyFlags() == 0);
    Chunk& neighbor = world.insertChunk({.x = 1, .y = 0, .z = 0});
    neighbor.clearDirty(voxel::kDirtyMesh);
    CHECK(chunk.isDirty(voxel::kDirtyMesh)); // Its neighbor appeared.
    chunk.clearDirty(voxel::kDirtyMesh);

    world.setBlock({.x = 15, .y = 3, .z = 3}, 2);
    CHECK(chunk.isDirty(voxel::kDirtyMesh));
    CHECK(chunk.isDirty(voxel::kDirtySave));
    CHECK(neighbor.isDirty(voxel::kDirtyMesh));       // Its border changed...
    CHECK_FALSE(neighbor.isDirty(voxel::kDirtySave)); // ...but not its blocks.
    chunk.touch(42);
    CHECK(chunk.lastAccessTick() == 42);
}

TEST_CASE("Column summaries track the highest loaded block of every block column") {
    VoxelWorld world;
    world.insertChunk({.x = 2, .y = -1, .z = 0}, ChunkStorage(1)); // Stone up to y = -1.
    const ColumnSummary* column = world.findColumn({.x = 2, .z = 0});
    REQUIRE(column != nullptr);
    CHECK(column->highestBlock(0, 0) == -1);
    CHECK(column->highestBlock() == -1);
    CHECK(column->surfaceEstimate() == ColumnSummary::kNoBlock);
    world.setSurfaceEstimate({.x = 2, .z = 0}, 70);
    CHECK(column->surfaceEstimate() == 70);

    world.insertChunk({.x = 2, .y = 1, .z = 0}); // Air does not count.
    CHECK(column->highestBlock(5, 5) == -1);
    CHECK(std::ranges::equal(column->loadedChunkYs(), std::array{-1, 1}));

    world.insertChunk({.x = 2, .y = 0, .z = 0});
    world.setBlock({.x = 32 + 3, .y = 5, .z = 4}, 3);
    world.setBlock({.x = 32 + 3, .y = 9, .z = 4}, 3);
    CHECK(column->highestBlock(3, 4) == 9);
    CHECK(column->highestBlock() == 9);
    world.setBlock({.x = 32 + 3, .y = 9, .z = 4}, voxel::kAirState); // Falls back to the next one down.
    CHECK(column->highestBlock(3, 4) == 5);
    world.setBlock({.x = 32 + 3, .y = 5, .z = 4}, voxel::kAirState); // And down into the stone chunk.
    CHECK(column->highestBlock(3, 4) == -1);
    world.setBlock({.x = 32 + 3, .y = 20, .z = 4}, 7); // Placed in the chunk above.
    CHECK(column->highestBlock(3, 4) == 20);
    world.removeChunk({.x = 2, .y = 1, .z = 0}); // Unloading it drops that height.
    CHECK(column->highestBlock(3, 4) == -1);

    // Replacing a chunk can lower and raise heights.
    world.insertChunk({.x = 2, .y = -1, .z = 0}); // The stone becomes air.
    CHECK(column->highestBlock(0, 0) == ColumnSummary::kNoBlock);
    world.insertChunk({.x = 2, .y = 0, .z = 0}, ChunkStorage(1));
    CHECK(column->highestBlock(0, 0) == 15);

    world.removeChunk({.x = 2, .y = 0, .z = 0});
    world.removeChunk({.x = 2, .y = -1, .z = 0});
    CHECK(world.findColumn({.x = 2, .z = 0}) == nullptr); // The last chunk took the column with it.
}

TEST_CASE("Column summaries match a brute force scan after random work") {
    VoxelWorld world;
    std::mt19937 random(99); // NOLINT(bugprone-random-generator-seed): the same edits every run.
    std::uniform_int_distribution<i32> chunkY(-2, 5);
    std::uniform_int_distribution<i32> local(0, 15);
    std::uniform_int_distribution<i32> action(0, 9);
    for (i32 y = -2; y <= 5; ++y) {
        world.insertChunk({.x = 0, .y = y, .z = 0}, sampleStorage({.x = 0, .y = y, .z = 0}));
    }
    for (int step = 0; step < 3000; ++step) {
        const i32 cy = chunkY(random);
        const int kind = action(random);
        if (kind == 0) {
            world.removeChunk({.x = 0, .y = cy, .z = 0});
        } else if (kind == 1) {
            world.insertChunk({.x = 0, .y = cy, .z = 0}, sampleStorage({.x = 0, .y = cy - 1, .z = 0}));
        } else {
            const BlockStateId state = kind < 6 ? voxel::kAirState : static_cast<BlockStateId>(kind);
            world.setBlock({.x = local(random), .y = (cy * 16) + local(random), .z = local(random)}, state);
        }
    }
    const ColumnSummary* column = world.findColumn({});
    REQUIRE(column != nullptr);
    for (i32 z = 0; z < 16; ++z) {
        for (i32 x = 0; x < 16; ++x) {
            REQUIRE(column->highestBlock(static_cast<u32>(x), static_cast<u32>(z)) ==
                    bruteForceHighest(world, x, z, -32, 95));
        }
    }
}

TEST_CASE("Snapshots copy the chunk and a one block border from every neighbor") {
    VoxelWorld world;
    ChunkSnapshot snapshot;
    CHECK_FALSE(world.createSnapshot({}, snapshot)); // Not loaded.

    // A mixed center, mixed, uniform and missing neighbors, at a far position.
    const ChunkPos center{.x = 625000, .y = -3, .z = -625000};
    world.insertChunk(center, sampleStorage(test::sampleChunkWithBits(4)));
    u16 seed = 0;
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                ++seed;
                if ((dx == 0 && dy == 0 && dz == 0) || seed % 5 == 0) {
                    continue; // Every fifth neighbor stays unloaded.
                }
                world.insertChunk(center.offset(dx, dy, dz),
                                  seed % 3 == 0 ? ChunkStorage(seed) : patternStorage(seed));
            }
        }
    }
    REQUIRE(world.createSnapshot(center, snapshot, 999));
    CHECK(snapshot.position == center);
    CHECK(snapshot.version == world.findChunk(center)->version());
    const BlockPos origin = chunkOrigin(center);
    for (i32 y = -1; y <= kChunkSize; ++y) {
        for (i32 z = -1; z <= kChunkSize; ++z) {
            for (i32 x = -1; x <= kChunkSize; ++x) {
                const BlockStateId expected =
                    world.getBlock({.x = origin.x + x, .y = origin.y + y, .z = origin.z + z}, 999);
                REQUIRE(snapshot.at(x, y, z) == expected);
            }
        }
    }
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const bool loaded =
                    (dx != 0 || dy != 0 || dz != 0) && world.findChunk(center.offset(dx, dy, dz)) != nullptr;
                CHECK(snapshot.hasNeighbor(dx, dy, dz) == loaded);
            }
        }
    }
}

TEST_CASE("Snapshots at the edge of the chunk grid treat the outside as unloaded") {
    VoxelWorld world;
    const ChunkPos edge{.x = INT32_MAX, .y = INT32_MIN, .z = 0};
    world.insertChunk(edge, ChunkStorage(3));
    auto snapshot = std::make_unique<ChunkSnapshot>();
    REQUIRE(world.createSnapshot(edge, *snapshot, 5));
    CHECK(snapshot->at(16, 4, 4) == 5);
    CHECK(snapshot->at(4, -1, 4) == 5);
    CHECK(snapshot->at(4, 4, 4) == 3);
    CHECK(snapshot->neighbors == 0);
    CHECK(world.setBlock({.x = (i64{INT32_MAX} * 16) + 15, .y = i64{INT32_MIN} * 16, .z = 0}, 2));
}

TEST_CASE("Storage counts and stats follow every change") {
    stats::StatRegistry registry;
    VoxelWorld world({}, &registry);
    world.insertChunk({}, ChunkStorage(1));
    world.insertChunk({.x = 1, .y = 0, .z = 0}, sampleStorage(test::sampleChunkWithBits(4)));
    const VoxelWorld::StorageCounts& counts = world.storageCounts();
    CHECK(counts.uniform == 1);
    CHECK(counts.palette[2] == 1);               // The terrain surface chunk uses 4 bit indices.
    world.setBlock({.x = 3, .y = 3, .z = 3}, 2); // Uniform to a 1 bit palette.
    CHECK(counts.uniform == 0);
    CHECK(counts.palette[0] == 1);
    usize bytes = 0;
    world.forEachChunk([&](const Chunk& chunk) { bytes += chunk.blocks().memoryBytes(); });
    CHECK(counts.blockBytes == bytes);

    auto snapshot = std::make_unique<ChunkSnapshot>();
    REQUIRE(world.createSnapshot({}, *snapshot));
    world.publishStats();
    registry.endFrame(0.016);
    CHECK(registry.value(registry.find("Voxel.World.Chunks")) == 2.0);
    CHECK(registry.value(registry.find("Voxel.World.Palette1Chunks")) == 1.0);
    CHECK(registry.value(registry.find("Voxel.World.BlockBytes")) == static_cast<f64>(bytes));
    CHECK(registry.value(registry.find("Voxel.World.Columns")) == 2.0);
    CHECK(registry.value(registry.find("Voxel.World.Snapshots")) == 1.0);

    world.clear();
    CHECK(counts.blockBytes == 0);
    CHECK(counts.palette[0] == 0);
    CHECK(counts.palette[2] == 0);
}

// E9.3 acceptance: a snapshot with uniform neighbors takes under 5
// microseconds. The center is a terrain surface chunk (4 bit palette), the
// slowest center the sample has; the neighbors are all air or all stone.
TEST_CASE("A snapshot with uniform neighbors takes under 5 microseconds") {
    VoxelWorld world;
    world.insertChunk({}, sampleStorage(test::sampleChunkWithBits(4)));
    REQUIRE(world.findChunk({})->blocks().bitsPerIndex() == 4);
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    world.insertChunk({.x = dx, .y = dy, .z = dz},
                                      ChunkStorage(dy < 0 ? BlockStateId{1} : voxel::kAirState));
                }
            }
        }
    }
    auto snapshot = std::make_unique<ChunkSnapshot>();
    constexpr int kRuns = 2000;
    f64 best = 1e9;
    int created = 0;
    for (int batch = 0; batch < 5; ++batch) {
        const Stopwatch stopwatch;
        for (int i = 0; i < kRuns; ++i) {
            // Counted instead of checked: a doctest assertion per call would
            // cost more than the snapshot itself.
            created += world.createSnapshot({}, *snapshot) ? 1 : 0;
        }
        best = std::min(best, stopwatch.elapsedMilliseconds() * 1000.0 / kRuns);
    }
    CHECK(created == 5 * kRuns);
    MESSAGE("Snapshot with uniform neighbors: " << best << " us");
#if defined(NDEBUG) && !defined(VXE_SANITIZER_ACTIVE)
    CHECK(best < (test::timingIsReliable() ? 5.0 : 50.0));
#endif
}
