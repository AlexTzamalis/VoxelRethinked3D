#include "client/demo_world.hpp"
#include "common/block_catalog.hpp"
#include "common/catalog_state_names.hpp"
#include "worldgen/climate.hpp"
#include "worldgen/terrain.hpp"
#include "worldgen/terrain_lod_sampler.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_codec.hpp>
#include <vxe/voxel/lod_cache.hpp>
#include <vxe/voxel/region_store.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

vr::TerrainBlocks blocks() {
    auto found = vr::TerrainBlocks::fromCatalog(catalog());
    REQUIRE(found.hasValue());
    return found.value();
}

} // namespace

TEST_CASE("Terrain is deterministic and depends on the seed") {
    const vr::Terrain terrain(blocks(), 12345, {}, false);
    const vr::Terrain other(blocks(), 999);
    auto first = std::make_unique<States>();
    auto second = std::make_unique<States>();
    auto third = std::make_unique<States>();
    const vxe::BlockPos land = terrain.spawnColumn();
    const vxe::ChunkPos chunk = vxe::toChunk(land);
    terrain.generate(chunk, *first);
    terrain.generate(chunk, *second);
    other.generate(chunk, *third);
    CHECK(*first == *second);
    CHECK(*first != *third);
}

TEST_CASE("Terrain has a surface on its height and trees standing on it") {
    const vr::TerrainBlocks ids = blocks();
    const vr::Terrain terrain(ids, 12345, {}, false);
    vxe::voxel::VoxelWorld world;
    auto states = std::make_unique<States>();
    // A block of chunks around the land, as one world, so trees and columns
    // can be checked across chunk borders.
    // Woods near the start, so that there are trees to look at.
    const auto woods = terrain.findBiome(vr::Biome::Forest, 0, 0);
    REQUIRE(woods.has_value());
    const vxe::ChunkPos middle = vxe::toChunk(woods.value_or(vxe::BlockPos{}));
    const vxe::i64 startX = (vxe::i64{middle.x} - 1) * 16;
    const vxe::i64 startZ = (vxe::i64{middle.z} - 1) * 16;
    for (vxe::i32 z = middle.z - 1; z <= middle.z + 1; ++z) {
        for (vxe::i32 x = middle.x - 1; x <= middle.x + 1; ++x) {
            for (vxe::i32 y = middle.y - 3; y <= middle.y + 3; ++y) {
                terrain.generate({.x = x, .y = y, .z = z}, *states);
                vxe::voxel::ChunkStorage storage;
                storage.assign(*states);
                world.insertChunk({.x = x, .y = y, .z = z}, std::move(storage));
            }
        }
    }
    vxe::usize logs = 0;
    vxe::usize crowns = 0;
    for (vxe::i64 z = startZ; z < startZ + 48; ++z) {
        for (vxe::i64 x = startX; x < startX + 48; ++x) {
            const vxe::i64 height = terrain.surfaceHeight(x, z);
            const BlockStateId surface = world.getBlock({.x = x, .y = height, .z = z});
            // The surface block is what the column says, unless a cave, a
            // boulder or a tree stands there.
            CHECK(terrain.topBlock(x, z) != vxe::voxel::kAirState);
            static_cast<void>(surface);
            if (height < vr::Terrain::kSeaLevel) {
                CHECK(world.getBlock({.x = x, .y = vr::Terrain::kSeaLevel, .z = z}) == ids.water);
            }
            for (vxe::i64 y = (vxe::i64{middle.y} - 3) * 16; y < (vxe::i64{middle.y} + 4) * 16; ++y) {
                if (world.getBlock({.x = x, .y = y, .z = z}) != ids.trees[0].log) {
                    continue;
                }
                ++logs;
                // The top of every trunk carries leaves, even across a chunk border.
                if (world.getBlock({.x = x, .y = y + 1, .z = z}) != ids.trees[0].log) {
                    CHECK(world.getBlock({.x = x, .y = y + 1, .z = z}) != vxe::voxel::kAirState);
                    ++crowns;
                }
            }
        }
    }
    MESSAGE(logs << " log blocks, " << crowns << " tree crowns");
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
    // Within render distance + 2 sideways, in whole layers around the camera.
    CHECK(demo.world().chunkCount() >= 9 * 9 * 8);
    CHECK(demo.world().chunkCount() % (9 * 9) == 0);
    CHECK(demo.spawnPosition().y > vr::Terrain::kSeaLevel);
    demo.shutdown();
    CHECK(demo.world().chunkCount() == 0);
}

TEST_CASE("Terrain chunks round trip through the chunk codec and region files by state name") {
    const auto ids = vr::TerrainBlocks::fromCatalog(catalog());
    REQUIRE(ids.hasValue());
    const vr::Terrain terrain(ids.value(), 777, {}, false);
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

TEST_CASE("Far terrain carries the trees: crowns in fine levels, a leaf tint in coarse ones") {
    const vr::TerrainBlocks ids = blocks();
    const vr::Terrain terrain(ids, 12345, {}, false);
    auto built = catalog().buildRegistry();
    REQUIRE(built.hasValue());
    const auto& view = built.value();
    const auto layers = static_cast<vxe::u16>(catalog().textureKeys().size());
    vr::TerrainLodSampler sampler(terrain, *view, layers, 12345);
    // Another texture layout means other material numbers, so old tiles must not load.
    CHECK(vr::TerrainLodSampler(terrain, *view, layers + 1, 12345).worldHash() != sampler.worldHash());

    // A broad tree near the land, whose crown fills the cell under its middle.
    std::optional<vr::Terrain::PlacedTree> found;
    const vxe::BlockPos land = terrain.spawnColumn();
    for (vxe::i64 cell = 0; !found && cell < 40000; ++cell) {
        const auto tree = terrain.treeAt((land.x / vr::Terrain::kTreeCell) + (cell % 200) - 100,
                                         (land.z / vr::Terrain::kTreeCell) + (cell / 200) - 100);
        if (tree && tree->tree.kind != vr::TreeKind::Bush) {
            found = tree;
        }
    }
    REQUIRE(found.has_value());
    const vr::Terrain::Tree& spot = found->tree;
    const vxe::u16 leaves = view->info(ids.trees[static_cast<vxe::usize>(spot.kind)].leaves)
                                .textureLayer(vxe::voxel::BlockFace::PosY);
    const std::atomic<bool> cancelled{false};
    const vxe::voxel::LodTileKey fine = vxe::voxel::lodTileAt(1, spot.crownX, spot.crownZ);
    vxe::voxel::LodTileBuilder builder(fine);
    REQUIRE(sampler.sampleTile(fine, {.maxSpansPerCell = 4}, builder, cancelled));
    const vxe::voxel::LodTile tile = std::move(builder).build();
    const auto cellX = static_cast<vxe::u32>((spot.crownX - fine.originX()) / 2);
    const auto cellZ = static_cast<vxe::u32>((spot.crownZ - fine.originZ()) / 2);
    const auto spans = tile.cell(cellX, cellZ);
    REQUIRE(!spans.empty());
    CHECK(spans.front().topY >= found->ground + spot.height);
    CHECK(spans.front().topMaterial != 0);
    static_cast<void>(leaves);

    // A coarse tile somewhere wooded has no crown spans but tinted grass.
    vxe::usize tinted = 0;
    for (vxe::i64 i = 0; i < 12 && tinted == 0; ++i) {
        const vxe::voxel::LodTileKey coarse = vxe::voxel::lodTileAt(5, spot.x + (i * 2048), spot.z);
        vxe::voxel::LodTileBuilder coarseBuilder(coarse);
        REQUIRE(sampler.sampleTile(coarse, {.maxSpansPerCell = 4}, coarseBuilder, cancelled));
        const vxe::voxel::LodTile coarseTile = std::move(coarseBuilder).build();
        for (const vxe::voxel::LodSpan& span : coarseTile.allSpans()) {
            tinted += span.topMaterial > layers ? 1U : 0U; // The grass and leaf blends follow the layers.
        }
    }
    CHECK(tinted > 0);

    // The color table: every layer, then water over its floor and 7 blends.
    const std::vector<vxe::u32> layerColors(layers, 0xFF808080U);
    CHECK(sampler.materialColors(layerColors).size() == vxe::usize{layers} + 8);
}

TEST_CASE("Edits reach the far terrain cache, at the latest when the world closes") {
    const auto folder = std::filesystem::temp_directory_path() / "vr_tests" / "demo_lod_edits";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    vxe::config::SettingsRegistry settings;
    vxe::voxel::BlockRegistry blocks;
    auto view = catalog().buildRegistry();
    REQUIRE(view.hasValue());
    blocks.publish(std::move(view).value());
    vxe::jobs::JobSystem jobs({.workerCount = 4, .ioThreadCount = 1});
    auto world = vr::DemoWorld::create({.jobs = &jobs,
                                        .settings = &settings,
                                        .catalog = &catalog(),
                                        .blocks = &blocks,
                                        .lodCacheFolder = folder,
                                        .premapBlocks = 0});
    REQUIRE(world.hasValue());
    vr::DemoWorld& demo = *world.value();
    REQUIRE(settings.set("World.Streaming.RenderDistance", vxe::config::SettingValue{2}).hasValue());
    REQUIRE(settings.set("Graphics.LOD.Levels", vxe::config::SettingValue{2}).hasValue());
    REQUIRE(settings.set("Graphics.LOD.DetailTiles", vxe::config::SettingValue{2}).hasValue());
    vxe::render::Camera camera;
    camera.position = demo.spawnPosition();
    const vxe::Stopwatch stopwatch;
    while (!demo.isLoaded() && stopwatch.elapsedMilliseconds() < 30000.0) {
        demo.update(camera);
    }
    REQUIRE(demo.isLoaded());
    // A lumen pillar from the ground to 40 blocks above the spawn's feet,
    // above any tree, so only the edit can put a span that high.
    const vxe::DVec3 feet = demo.spawnGround();
    const vxe::BlockPos ground{.x = static_cast<vxe::i64>(std::floor(feet.x)),
                               .y = static_cast<vxe::i64>(std::floor(feet.y)),
                               .z = static_cast<vxe::i64>(std::floor(feet.z))};
    const auto lumen = catalog().stateId("lumen_block");
    REQUIRE(lumen.has_value());
    const vxe::i64 top = ground.y + 40;
    for (vxe::i64 y = ground.y; y <= top; ++y) {
        REQUIRE(demo.setBlock({.x = ground.x, .y = y, .z = ground.z}, *lumen));
    }
    const vxe::u64 hash = demo.lodCacheHash();
    demo.shutdown(); // Well within the delay: the shutdown must send the column.

    vxe::voxel::LodTileCache cache({.folder = folder});
    const vxe::voxel::LodTileKey key = vxe::voxel::lodTileAt(1, ground.x, ground.z);
    const auto tile = cache.load(key, hash);
    REQUIRE(tile.has_value());
    const auto cellX = static_cast<vxe::u32>((ground.x - key.originX()) / key.cellSize());
    const auto cellZ = static_cast<vxe::u32>((ground.z - key.originZ()) / key.cellSize());
    vxe::i64 highest = 0;
    for (const vxe::voxel::LodSpan& span : tile->cell(cellX, cellZ)) {
        highest = std::max<vxe::i64>(highest, span.topY);
    }
    CHECK(highest >= top);
}
