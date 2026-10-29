#include "client/demo_world.hpp"
#include "common/block_catalog.hpp"
#include "worldgen/demo_terrain.hpp"

#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <array>
#include <memory>

using vxe::voxel::BlockStateId;
using States = std::array<BlockStateId, vxe::kChunkVolume>;

namespace {

const vr::BlockCatalog& catalog() {
    static const vr::BlockCatalog kCatalog = [] {
        auto loaded = vr::BlockCatalog::loadFolder(VR_BLOCKS_DIR);
        REQUIRE(loaded.hasValue());
        return std::move(loaded).value();
    }();
    return kCatalog;
}

vr::DemoBlocks blocks() {
    auto found = vr::DemoBlocks::fromCatalog(catalog());
    REQUIRE(found.hasValue());
    return found.value();
}

} // namespace

TEST_CASE("Demo terrain is deterministic and depends on the seed") {
    const vr::DemoTerrain terrain(blocks(), 12345);
    const vr::DemoTerrain other(blocks(), 999);
    auto first = std::make_unique<States>();
    auto second = std::make_unique<States>();
    auto third = std::make_unique<States>();
    const vxe::ChunkPos chunk{.x = 3, .y = 3, .z = -2};
    terrain.generate(chunk, *first);
    terrain.generate(chunk, *second);
    other.generate(chunk, *third);
    CHECK(*first == *second);
    CHECK(*first != *third);
}

TEST_CASE("Demo terrain has a floor, a surface on its height and water at sea level") {
    const vr::DemoBlocks ids = blocks();
    const vr::DemoTerrain terrain(ids, 12345);
    vxe::voxel::VoxelWorld world;
    auto states = std::make_unique<States>();
    // A 3 x 8 x 3 block of chunks around the origin, as one world, so trees
    // and columns can be checked across chunk borders.
    for (vxe::i32 z = -1; z <= 1; ++z) {
        for (vxe::i32 x = -1; x <= 1; ++x) {
            for (vxe::i32 y = 0; y < 8; ++y) {
                terrain.generate({.x = x, .y = y, .z = z}, *states);
                vxe::voxel::ChunkStorage storage;
                storage.assign(*states);
                world.insertChunk({.x = x, .y = y, .z = z}, std::move(storage));
            }
        }
    }
    vxe::usize logs = 0;
    vxe::usize crowns = 0;
    vxe::usize water = 0;
    for (vxe::i64 z = -16; z < 32; ++z) {
        for (vxe::i64 x = -16; x < 32; ++x) {
            CHECK(world.getBlock({.x = x, .y = 0, .z = z}) == ids.floor);
            const vxe::i64 height = terrain.surfaceHeight(x, z);
            const BlockStateId surface = world.getBlock({.x = x, .y = height, .z = z});
            // The surface block is grass, sand or snow unless a cave or a
            // trunk took its place.
            const bool natural = surface == ids.grass || surface == ids.sand || surface == ids.snow;
            CHECK((natural || surface == vxe::voxel::kAirState || surface == ids.log));
            if (height < vr::DemoTerrain::kSeaLevel) {
                CHECK(world.getBlock({.x = x, .y = vr::DemoTerrain::kSeaLevel, .z = z}) == ids.water);
                ++water;
            }
            for (vxe::i64 y = 1; y < 127; ++y) {
                if (world.getBlock({.x = x, .y = y, .z = z}) != ids.log) {
                    continue;
                }
                ++logs;
                // The top of every trunk carries leaves, even across a chunk border.
                if (world.getBlock({.x = x, .y = y + 1, .z = z}) != ids.log) {
                    CHECK(world.getBlock({.x = x, .y = y + 1, .z = z}) == ids.leaves);
                    ++crowns;
                }
            }
        }
    }
    MESSAGE(logs << " log blocks, " << crowns << " tree crowns, " << water << " water columns");
    CHECK(crowns > 0);
}

TEST_CASE("The demo world generates and meshes on the job system without a renderer") {
    vxe::jobs::JobSystem jobs({.workerCount = 4, .ioThreadCount = 1});
    auto view = catalog().buildRegistry();
    REQUIRE(view.hasValue());
    auto world = vr::DemoWorld::create(
        {.jobs = &jobs, .catalog = &catalog(), .blocks = view.value(), .radiusChunks = 2, .heightChunks = 8});
    REQUIRE(world.hasValue());
    vr::DemoWorld& demo = *world.value();
    const vxe::Stopwatch stopwatch;
    while (demo.phase() != vr::DemoWorld::Phase::Ready && stopwatch.elapsedMilliseconds() < 30000.0) {
        demo.advance();
        demo.upload();
    }
    CHECK(demo.phase() == vr::DemoWorld::Phase::Ready);
    CHECK(demo.world().chunkCount() == 4 * 4 * 8);
    CHECK(demo.spawnPosition().y > vr::DemoTerrain::kSeaLevel);
    demo.shutdown();
}
