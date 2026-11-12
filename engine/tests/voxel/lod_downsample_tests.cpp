#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/lod_downsample.hpp>
#include <vxe/voxel/lod_manager.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace vxe;
using voxel::LodSpan;
using voxel::LodTileKey;

namespace {

constexpr voxel::BlockStateId kStone = 1;
constexpr voxel::BlockStateId kGrass = 2;

// Stone shows texture layer 5 on every face; grass 7 on top and 8 on the sides.
std::shared_ptr<const voxel::BlockRegistryView> testBlocks() {
    voxel::BlockRegistryBuilder builder;
    voxel::BlockRenderInfo stone{.layer = voxel::RenderLayer::Opaque};
    stone.faceTextureLayer.fill(5);
    voxel::BlockRenderInfo grass{.layer = voxel::RenderLayer::Opaque};
    grass.faceTextureLayer.fill(8);
    grass.faceTextureLayer[static_cast<usize>(voxel::BlockFace::PosY)] = 7;
    REQUIRE(builder.add(stone).hasValue());
    REQUIRE(builder.add(grass).hasValue());
    return builder.build();
}

LodSpan span(i32 bottom, i32 top, u16 material = 1) {
    return {.bottomY = bottom, .topY = top, .topMaterial = material, .sideMaterial = material};
}

std::vector<LodSpan> collect(std::span<const LodSpan> spans) {
    return {spans.begin(), spans.end()};
}

} // namespace

TEST_CASE("Too many LOD spans join across the smallest gaps") {
    std::vector<LodSpan> spans = {span(90, 100), span(70, 80), span(60, 68), span(0, 10)};
    voxel::limitLodSpans(spans, 3);
    CHECK(spans == std::vector{span(90, 100), span(60, 80), span(0, 10)});
    voxel::limitLodSpans(spans, 1);
    CHECK(spans == std::vector{span(0, 100)});
}

TEST_CASE("A chunk column becomes level 1 cells: runs of blocks with the top block's materials") {
    const auto blocks = testBlocks();
    // Chunk 0: stone up to y 9 with grass on y 10 everywhere; one 2 x 2 cell
    // (cells x 0, z 0) also has a floating stone block at y 14.
    voxel::ChunkStorage ground;
    for (u8 y = 0; y <= 10; ++y) {
        for (u8 z = 0; z < 16; ++z) {
            for (u8 x = 0; x < 16; ++x) {
                ground.set({.x = x, .y = y, .z = z}, y == 10 ? kGrass : kStone);
            }
        }
    }
    ground.set({.x = 1, .y = 14, .z = 1}, kStone);
    const std::array<const voxel::ChunkStorage*, 2> chunks = {&ground, nullptr};
    std::array<std::vector<LodSpan>, 64> cells;
    voxel::downsampleChunkColumn(chunks, 3, *blocks, 4, cells); // Chunk y 3: blocks 48 to 79.
    const std::vector<LodSpan> plain = {{.bottomY = 48, .topY = 58, .topMaterial = 7, .sideMaterial = 8}};
    CHECK(cells.at(1) == plain);
    CHECK(cells.at(63) == plain);
    const std::vector<LodSpan> island = {{.bottomY = 62, .topY = 62, .topMaterial = 5, .sideMaterial = 5},
                                         {.bottomY = 48, .topY = 58, .topMaterial = 7, .sideMaterial = 8}};
    CHECK(cells.at(0) == island);
    // One span allowed: the island joins the ground.
    voxel::downsampleChunkColumn(chunks, 3, *blocks, 1, cells);
    CHECK(cells.at(0).size() == 1);
    CHECK(cells.at(0).front().topY == 62);
    CHECK(cells.at(0).front().bottomY == 48);
}

TEST_CASE("Replacing a height range keeps the spans outside it") {
    const std::vector<LodSpan> old = {span(200, 210, 3), span(20, 100, 1)};
    std::vector<LodSpan> out;
    // Blocks 48 to 79 were loaded and now hold ground up to 70, which joins
    // the old ground below the range and gives it its top.
    voxel::replaceLodSpanRange(old, std::vector{span(48, 70, 2)}, 48, 79, 4, out);
    CHECK(out == std::vector{span(200, 210, 3), span(80, 100, 1), span(20, 70, 2)});
    // Dug away completely inside the range.
    voxel::replaceLodSpanRange(old, {}, 48, 79, 4, out);
    CHECK(out == std::vector{span(200, 210, 3), span(80, 100, 1), span(20, 47, 1)});
}

TEST_CASE("A coarser cell is the union of its four cells, colored from the highest") {
    const std::vector<LodSpan> a = {span(0, 50, 1)};
    const std::vector<LodSpan> b = {span(0, 60, 2)};
    const std::vector<LodSpan> c = {span(90, 95, 3), span(0, 40, 1)};
    const std::vector<LodSpan> d;
    std::vector<LodSpan> out;
    voxel::downsampleLodCells({a, b, c, d}, 4, out);
    CHECK(out == std::vector{span(90, 95, 3), span(0, 60, 2)});

    // A whole tile into its parent's quadrant.
    voxel::LodTileBuilder child({.level = 1, .x = 3, .z = -1}); // Odd x, odd z: the +X +Z quadrant.
    for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
        const LodSpan cell = span(0, static_cast<i32>(i % 64), 4);
        child.addCell(std::span(&cell, 1));
    }
    const voxel::LodTile childTile = std::move(child).build();
    voxel::LodTileBuilder parent(childTile.key().parent());
    for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
        parent.addCell(std::vector{span(0, 1, 9)});
    }
    voxel::LodTile parentTile = std::move(parent).build();
    voxel::downsampleLodTile(childTile, parentTile, 4);
    CHECK(collect(parentTile.cell(0, 0)) == std::vector{span(0, 1, 9)});    // Untouched quadrant.
    CHECK(collect(parentTile.cell(32, 32)) == std::vector{span(0, 1, 4)});  // Child cells 0 and 1.
    CHECK(collect(parentTile.cell(63, 63)) == std::vector{span(0, 63, 4)}); // Child cells 62 and 63.
}

namespace {

// Flat ground at y 20 everywhere.
class FlatSampler final : public voxel::LodTileSampler {
public:
    bool sampleTile(const LodTileKey& /*key*/, const voxel::LodSampleOptions& /*options*/,
                    voxel::LodTileBuilder& builder, const std::atomic<bool>& /*cancelled*/) override {
        const LodSpan ground = span(0, 20, 1);
        for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
            builder.addCell(std::span(&ground, 1));
        }
        return true;
    }
    [[nodiscard]] u64 worldHash() const override { return 5; }
};

class RecordingSink final : public voxel::LodTileSink {
public:
    u32 adds = 0;
    void addTile(std::shared_ptr<const voxel::LodTile> /*tile*/) override { ++adds; }
    void removeTile(const LodTileKey& /*key*/) override {}
};

} // namespace

TEST_CASE("An edited chunk column reaches every LOD level, on screen and in the cache") {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "lod_edit";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    config::SettingsRegistry settings;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 4, .ioThreadCount = 2}};
    FlatSampler sampler;
    RecordingSink sink;
    voxel::LodTileCache cache({.folder = folder});
    voxel::LodManager manager(
        {.jobs = &jobs, .settings = &settings, .sampler = &sampler, .cache = &cache, .sink = &sink});
    const DVec3 camera(8.0, 50.0, 8.0);
    const auto settle = [&] {
        const Stopwatch clock;
        do {
            manager.update(camera);
            std::this_thread::yield();
        } while (!manager.isSettled() && clock.elapsedMilliseconds() < 20000.0);
        REQUIRE(manager.isSettled());
    };
    settle();

    // A tower up to y 99 on the column of chunk (5, -3), loaded chunks 0 to 6.
    voxel::VoxelWorld world;
    const auto blocks = testBlocks();
    for (i32 y = 0; y <= 6; ++y) {
        voxel::ChunkStorage storage;
        for (u8 ly = 0; ly < 16; ++ly) {
            const i32 worldY = (y * 16) + ly;
            for (u8 z = 0; z < 16; ++z) {
                for (u8 x = 0; x < 16; ++x) {
                    const bool tower = x < 4 && z < 4 && worldY <= 99;
                    if (worldY <= 20 || tower) {
                        storage.set({.x = x, .y = ly, .z = z}, kStone);
                    }
                }
            }
        }
        world.insertChunk({.x = 5, .y = y, .z = -3}, storage);
    }
    const u32 addsBefore = sink.adds;
    const Stopwatch editClock;
    manager.columnEdited(world, {.x = 5, .z = -3}, blocks);
    CHECK(manager.pendingEdits() == 1);
    settle();
    MESSAGE("Column edit in all 6 levels (cached tiles) after " << editClock.elapsedMilliseconds() << " ms");
    CHECK(manager.pendingEdits() == 0);

    // Every level's tile over the tower has it in the cache; the shown ones
    // (level 1 here: the coarser tiles there lie in the finer level's hole)
    // show it too.
    const i64 x = i64{5} * 16;
    const i64 z = i64{-3} * 16;
    const u64 hash = voxel::lodCacheHash(sampler.worldHash(), manager.sampleOptions());
    u32 shown = 0;
    for (u8 level = 1; level <= 6; ++level) {
        const LodTileKey key = voxel::lodTileAt(level, x, z);
        const auto cellX = static_cast<u32>((x - key.originX()) / key.cellSize());
        const auto cellZ = static_cast<u32>((z - key.originZ()) / key.cellSize());
        const auto cached = cache.load(key, hash);
        REQUIRE(cached.has_value());
        CHECK(cached->cell(cellX, cellZ).front().topY == 99);
        if (const auto tile = manager.tile(key); tile != nullptr) {
            ++shown;
            CHECK(tile->cell(cellX, cellZ).front().topY == 99);
        }
    }
    CHECK(shown >= 1);
    CHECK(manager.tile(voxel::lodTileAt(1, x, z)) != nullptr);
    // A neighbor cell outside the tower keeps the ground.
    const auto level1 = manager.tile(voxel::lodTileAt(1, x, z));
    const auto groundX = static_cast<u32>((x + 8 - level1->key().originX()) / 2);
    const auto groundZ = static_cast<u32>((z - level1->key().originZ()) / 2);
    CHECK(level1->cell(groundX, groundZ).front().topY == 20);
    CHECK(sink.adds == addsBefore + shown);
}

TEST_CASE("finishEdits writes every waiting edit to the cache before a manager goes away") {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "lod_finish_edits";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    config::SettingsRegistry settings;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 2, .ioThreadCount = 1}};
    FlatSampler sampler;
    voxel::LodTileCache cache({.folder = folder});
    voxel::VoxelWorld world;
    const auto blocks = testBlocks();
    // Two edited columns, each a pillar up to y 40.
    for (const i32 column : {0, 3}) {
        voxel::ChunkStorage storage;
        for (u8 ly = 0; ly < 16; ++ly) {
            for (u8 z = 0; z < 16; ++z) {
                for (u8 x = 0; x < 16; ++x) {
                    storage.set({.x = x, .y = ly, .z = z}, kStone);
                }
            }
        }
        world.insertChunk({.x = column, .y = 2, .z = 0}, storage);
        world.insertChunk({.x = column, .y = 1, .z = 0}, storage);
    }
    u64 hash = 0;
    {
        voxel::LodManager manager(
            {.jobs = &jobs, .settings = &settings, .sampler = &sampler, .cache = &cache});
        manager.update(DVec3(8.0, 50.0, 8.0));
        hash = voxel::lodCacheHash(sampler.worldHash(), manager.sampleOptions());
        manager.columnEdited(world, {.x = 0, .z = 0}, blocks);
        manager.columnEdited(world, {.x = 3, .z = 0}, blocks);
        CHECK(manager.pendingEdits() == 2);
        manager.finishEdits();
        CHECK(manager.pendingEdits() == 0);
    }
    for (const i64 x : {i64{0}, i64{48}}) {
        const LodTileKey key = voxel::lodTileAt(1, x, 0);
        const auto cached = cache.load(key, hash);
        REQUIRE(cached.has_value());
        const auto cellX = static_cast<u32>((x - key.originX()) / key.cellSize());
        const auto cellZ = static_cast<u32>((0 - key.originZ()) / key.cellSize());
        CHECK(cached->cell(cellX, cellZ).front().topY == 47);
    }
}
