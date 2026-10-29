#include "client/demo_world.hpp"
#include "common/block_catalog.hpp"
#include "common/catalog_state_names.hpp"
#include "worldgen/demo_terrain.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_codec.hpp>
#include <vxe/voxel/region_store.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

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

TEST_CASE("The demo world streams in around the camera without a renderer") {
    vxe::config::SettingsRegistry settings;
    vxe::voxel::BlockRegistry blocks;
    auto view = catalog().buildRegistry();
    REQUIRE(view.hasValue());
    blocks.publish(std::move(view).value());
    vxe::jobs::JobSystem jobs({.workerCount = 4, .ioThreadCount = 1});
    auto world = vr::DemoWorld::create(
        {.jobs = &jobs, .settings = &settings, .catalog = &catalog(), .blocks = &blocks});
    REQUIRE(world.hasValue());
    vr::DemoWorld& demo = *world.value();
    REQUIRE(settings.set("World.Streaming.RenderDistance", vxe::config::SettingValue{2}).hasValue());
    vxe::render::Camera camera;
    camera.position = demo.spawnPosition();
    const vxe::Stopwatch stopwatch;
    while ((!demo.isLoaded() || !demo.streaming().isSettled()) && stopwatch.elapsedMilliseconds() < 30000.0) {
        demo.update(camera);
    }
    CHECK(demo.isLoaded());
    REQUIRE(demo.streaming().isSettled());
    // Within render distance + 2 sideways, all 8 layers of the world.
    CHECK(demo.world().chunkCount() == 9 * 9 * 8);
    CHECK(demo.spawnPosition().y > vr::DemoTerrain::kSeaLevel);
    demo.shutdown();
    CHECK(demo.world().chunkCount() == 0);
}

TEST_CASE("Demo terrain chunks round trip through the chunk codec and region files by state name") {
    const auto ids = vr::DemoBlocks::fromCatalog(catalog());
    REQUIRE(ids.hasValue());
    const vr::DemoTerrain terrain(ids.value(), 777);
    const vr::CatalogStateNames names(catalog());
    const auto folder = std::filesystem::temp_directory_path() / "vr_tests" / "regions";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    vxe::config::SettingsRegistry settings;
    vxe::voxel::RegionStore store({.folder = folder, .settings = &settings});

    // A column of chunks from the floor to the sky, across two regions.
    std::vector<vxe::voxel::ChunkStorage> originals;
    std::vector<std::vector<std::byte>> payloads;
    std::vector<vxe::voxel::RegionStore::SavedChunk> saved;
    std::array<vxe::voxel::BlockStateId, vxe::kChunkVolume> states{};
    vxe::usize bytes = 0;
    for (vxe::i32 y = 0; y < 8; ++y) {
        for (vxe::i32 x = 14; x < 18; ++x) {
            const vxe::ChunkPos position{.x = x, .y = y, .z = -3};
            terrain.generate(position, states);
            vxe::voxel::ChunkStorage blocks;
            blocks.assign(states);
            vxe::io::BinaryWriter writer;
            vxe::voxel::encodeChunk(blocks, names, writer);
            bytes += writer.size();
            payloads.push_back(writer.takeBytes());
            originals.push_back(std::move(blocks));
            saved.push_back({.position = position, .payload = payloads.back()});
        }
    }
    REQUIRE(store.save(saved).hasValue());
    std::vector<std::byte> loaded;
    for (vxe::usize i = 0; i < saved.size(); ++i) {
        auto found = store.load(saved[i].position, loaded);
        REQUIRE(found.hasValue());
        REQUIRE(found.value());
        auto decoded = vxe::voxel::decodeChunk(loaded, names);
        REQUIRE(decoded.hasValue());
        CHECK(decoded.value().unknownStates == 0);
        for (vxe::u16 block = 0; block < vxe::kChunkVolume; ++block) {
            REQUIRE(decoded.value().blocks.getIndex(block) == originals[i].getIndex(block));
        }
    }
    std::uintmax_t onDisk = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        onDisk += entry.file_size();
    }
    MESSAGE(saved.size() << " terrain chunks: " << bytes << " payload bytes, " << onDisk << " bytes on disk");
}
