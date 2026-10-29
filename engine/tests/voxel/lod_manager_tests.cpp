#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/lod_manager.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace vxe;
using voxel::LodManager;
using voxel::LodSpan;
using voxel::LodTileKey;

namespace {

// The ground height of a test world column.
i32 hillHeight(i64 x, i64 z) {
    return static_cast<i32>(64.0 + (20.0 * std::sin(static_cast<f64>(x) * 0.01)) +
                            (15.0 * std::cos(static_cast<f64>(z) * 0.013)));
}

// Hills, one span per cell, sampled at the cell's -X -Z corner. Can hold its
// jobs (to test cancellation) and counts what it was asked.
class HillSampler final : public voxel::LodTileSampler {
public:
    std::atomic<u64> hash{voxel::lodWorldHash(1, 1, 0)};
    std::atomic<u32> calls{0};
    std::atomic<u32> sawCancel{0};
    std::atomic<u32> lastMaxSpans{0};
    std::atomic<bool> hold{false};

    bool sampleTile(const LodTileKey& key, const voxel::LodSampleOptions& options,
                    voxel::LodTileBuilder& builder, const std::atomic<bool>& cancelled) override {
        calls.fetch_add(1);
        lastMaxSpans.store(options.maxSpansPerCell);
        while (hold.load() && !cancelled.load()) {
            std::this_thread::yield();
        }
        if (cancelled.load()) {
            sawCancel.fetch_add(1);
            return false;
        }
        for (i32 cz = 0; cz < voxel::kLodTileCells; ++cz) {
            for (i32 cx = 0; cx < voxel::kLodTileCells; ++cx) {
                const LodSpan span{.bottomY = -100,
                                   .topY = hillHeight(key.originX() + (cx * key.cellSize()),
                                                      key.originZ() + (cz * key.cellSize())),
                                   .topMaterial = 1,
                                   .sideMaterial = 2};
                builder.addCell(std::span(&span, 1));
            }
        }
        return true;
    }

    [[nodiscard]] u64 worldHash() const override { return hash.load(); }
};

// What the renderer would hold.
class TileSink final : public voxel::LodTileSink {
public:
    HashMap<i64, std::shared_ptr<const voxel::LodTile>> tiles; // By packed key.
    u32 adds = 0;
    u32 removes = 0;

    static i64 pack(const LodTileKey& key) {
        return (i64{key.level} << 56U) ^ (i64{key.x} << 28U) ^ (i64{key.z} & 0xFFFFFFF);
    }
    void addTile(std::shared_ptr<const voxel::LodTile> tile) override {
        ++adds;
        tiles.insert_or_assign(pack(tile->key()), std::move(tile));
    }
    void removeTile(const LodTileKey& key) override {
        ++removes;
        CHECK(tiles.erase(pack(key)) == 1);
    }
    [[nodiscard]] bool covers(i64 x, i64 z) const {
        return std::ranges::any_of(tiles, [&](const auto& entry) {
            const LodTileKey& key = entry.second->key();
            return x >= key.originX() && x < key.originX() + key.tileSize() && z >= key.originZ() &&
                   z < key.originZ() + key.tileSize();
        });
    }
};

std::filesystem::path freshFolder(std::string_view name) {
    const auto folder =
        std::filesystem::temp_directory_path() / "vxe_tests" / "lod_manager" / std::string(name);
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder);
    return folder;
}

struct Rig { // NOLINT(clang-analyzer-optin.performance.Padding): members in lifetime order.
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    HillSampler sampler;
    TileSink sink;
    std::unique_ptr<voxel::LodTileCache> cache;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 4, .ioThreadCount = 1}};
    std::unique_ptr<LodManager> manager;
    DVec3 camera{10.0, 80.0, -20.0};

    explicit Rig(const std::filesystem::path& cacheFolder = {}) {
        if (!cacheFolder.empty()) {
            cache = std::make_unique<voxel::LodTileCache>(
                voxel::LodTileCacheDesc{.folder = cacheFolder, .stats = &stats});
        }
        manager = std::make_unique<LodManager>(voxel::LodManagerDesc{.jobs = &jobs,
                                                                     .settings = &settings,
                                                                     .sampler = &sampler,
                                                                     .cache = cache.get(),
                                                                     .sink = &sink,
                                                                     .stats = &stats});
        set("Graphics.LOD.MaxJobsInFlight", 8);
    }
    ~Rig() { manager.reset(); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    void set(std::string_view id, const config::SettingValue& value) {
        REQUIRE(settings.set(id, value).hasValue());
    }
    void settle() const {
        const Stopwatch clock;
        do {
            manager->update(camera);
            std::this_thread::yield();
        } while (!manager->isSettled() && clock.elapsedMilliseconds() < 20000.0);
        REQUIRE(manager->isSettled());
    }
    // The sink holds exactly the clipmap's tiles.
    void checkShowsClipmap() const {
        std::set<i64> expected;
        for (const LodTileKey& key : manager->clipmap().tiles()) {
            expected.insert(TileSink::pack(key));
        }
        std::set<i64> shown;
        for (const auto& [packed, tile] : sink.tiles) {
            shown.insert(packed);
        }
        CHECK(shown == expected);
    }
};

} // namespace

TEST_CASE("The LOD manager samples every clipmap tile once and hands it to the sink") {
    Rig rig;
    rig.settle();
    const auto tiles = rig.manager->clipmap().tiles();
    CHECK(tiles.size() == 64 + (5 * 48));
    CHECK(rig.sampler.calls.load() == tiles.size());
    CHECK(rig.sink.adds == tiles.size());
    rig.checkShowsClipmap();
    const LodManager::Counts counts = rig.manager->counts();
    CHECK(counts.ready == tiles.size());
    CHECK(counts.readyPerLevel.at(1) == 64);
    CHECK(counts.readyPerLevel.at(6) == 48);
    CHECK(rig.sampler.lastMaxSpans.load() == 4);

    // The tiles hold what the sampler made.
    const LodTileKey key = tiles.back();
    const auto tile = rig.manager->tile(key);
    REQUIRE(tile != nullptr);
    CHECK(tile->cell(3, 5).front().topY ==
          hillHeight(key.originX() + (3 * key.cellSize()), key.originZ() + (5 * key.cellSize())));

    rig.stats.endFrame(1.0 / 60.0);
    CHECK(rig.stats.value(rig.stats.find("Voxel.Lod.Tiles")) == doctest::Approx(304.0));
    CHECK(rig.stats.value(rig.stats.find("Voxel.Lod.MemoryBytes")) > 304.0 * 4096.0 * 16.0);
}

TEST_CASE("LOD tiles come from the cache the second time, unless the options changed") {
    const auto folder = freshFolder("cache");
    {
        const Rig first(folder);
        first.settle(); // Includes the saves.
        CHECK(first.sampler.calls.load() == 304);
    }
    Rig second(folder);
    second.settle();
    CHECK(second.sampler.calls.load() == 0);
    second.checkShowsClipmap();
    second.stats.endFrame(1.0 / 60.0);
    CHECK(second.stats.value(second.stats.find("Voxel.Lod.FromCache")) == doctest::Approx(304.0));

    // More spans per cell: every tile is stale, in memory and on disk. The
    // old tiles stay shown until their replacements arrive.
    second.set("Graphics.LOD.MaxSpansPerCell", 6);
    second.manager->update(second.camera);
    CHECK(second.sink.tiles.size() == 304);
    second.settle();
    CHECK(second.sampler.calls.load() == 304);
    CHECK(second.sampler.lastMaxSpans.load() == 6);
    CHECK(second.sink.removes == 0); // Replaced in place.
    second.checkShowsClipmap();

    // Another generator version makes the cache stale as well.
    Rig third(folder);
    third.sampler.hash.store(voxel::lodWorldHash(1, 2, 0));
    third.settle();
    CHECK(third.sampler.calls.load() == 304);
}

TEST_CASE("Moving the camera never uncovers ground the LOD showed") {
    Rig rig;
    rig.settle();
    std::mt19937 random(3); // NOLINT(bugprone-random-generator-seed): the same path every run.
    const i64 reach = 3 * LodTileKey{.level = 6}.tileSize();
    for (int step = 0; step < 300; ++step) {
        if (step % 3 == 0) {
            rig.camera.x += 70.0;
            rig.camera.z -= 23.0;
        }
        // Ground shown now and within reach after the move stays shown.
        std::vector<std::pair<i64, i64>> points;
        for (int i = 0; i < 40; ++i) {
            const i64 x = static_cast<i64>(rig.camera.x) - reach + static_cast<i64>(random() % (2 * reach));
            const i64 z = static_cast<i64>(rig.camera.z) - reach + static_cast<i64>(random() % (2 * reach));
            if (rig.sink.covers(x, z)) {
                points.emplace_back(x, z);
            }
        }
        rig.manager->update(rig.camera);
        for (const auto& [x, z] : points) {
            CHECK(rig.sink.covers(x, z));
        }
        std::this_thread::yield();
    }
    rig.settle();
    rig.checkShowsClipmap();
    CHECK(rig.manager->counts().retiring == 0);
}

TEST_CASE("Turning far terrain off removes every tile; on brings them back") {
    Rig rig;
    rig.settle();
    rig.set("Graphics.LOD.Enabled", false);
    rig.manager->update(rig.camera);
    CHECK(rig.sink.tiles.empty());
    CHECK(rig.manager->counts().wanted == 0);
    rig.set("Graphics.LOD.Enabled", true);
    rig.settle();
    rig.checkShowsClipmap();

    rig.set("Graphics.LOD.Levels", 7);
    rig.settle();
    CHECK(rig.sink.tiles.size() == 64 + (6 * 48));
    rig.set("Graphics.LOD.HalfExtentTiles", 7); // Odd: counts as 6.
    rig.settle();
    CHECK(rig.sink.tiles.size() == 144 + (6 * 108));
    rig.checkShowsClipmap();
}

TEST_CASE("Tiles that leave the clipmap cancel their jobs") {
    Rig rig;
    rig.sampler.hold.store(true);
    rig.manager->update(rig.camera); // Jobs start and wait.
    const Stopwatch clock;
    while (rig.sampler.calls.load() == 0 && clock.elapsedMilliseconds() < 10000.0) {
        std::this_thread::yield();
    }
    REQUIRE(rig.sampler.calls.load() > 0);
    rig.camera.x += 5.0e6; // Far away: every tile changes.
    rig.manager->update(rig.camera);
    rig.sampler.hold.store(false);
    rig.settle();
    CHECK(rig.sampler.sawCancel.load() > 0);
    rig.checkShowsClipmap();
}
