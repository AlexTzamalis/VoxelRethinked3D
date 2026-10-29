#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/lod_premapper.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

using namespace vxe;
using voxel::LodPremapper;
using voxel::LodSpan;
using voxel::LodTileKey;

namespace {

class CountingSampler final : public voxel::LodTileSampler {
public:
    std::atomic<u32> calls{0};
    std::atomic<bool> slow{false};

    bool sampleTile(const LodTileKey& key, const voxel::LodSampleOptions& /*options*/,
                    voxel::LodTileBuilder& builder, const std::atomic<bool>& cancelled) override {
        calls.fetch_add(1);
        if (slow.load()) {
            const Stopwatch clock;
            while (!cancelled.load() && clock.elapsedMilliseconds() < 50.0) {
                std::this_thread::yield();
            }
            if (cancelled.load()) {
                return false;
            }
        }
        const LodSpan ground{.bottomY = 0, .topY = 60 + key.level, .topMaterial = 1, .sideMaterial = 2};
        for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
            builder.addCell(std::span(&ground, 1));
        }
        return true;
    }
    [[nodiscard]] u64 worldHash() const override { return 99; }
};

std::filesystem::path freshFolder(std::string_view name) {
    const auto folder =
        std::filesystem::temp_directory_path() / "vxe_tests" / "lod_premap" / std::string(name);
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder);
    return folder;
}

void runToEnd(LodPremapper& premapper) {
    const Stopwatch clock;
    do {
        premapper.update();
        std::this_thread::yield();
    } while (!premapper.isDone() && clock.elapsedMilliseconds() < 30000.0);
    REQUIRE(premapper.isDone());
}

} // namespace

TEST_CASE("Premapping bakes the clipmap tiles within the radius, nearest first, once") {
    const auto folder = freshFolder("bake");
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 4, .ioThreadCount = 2}};
    CountingSampler sampler;
    voxel::LodTileCache cache({.folder = folder});
    const voxel::LodPremapDesc desc{.jobs = &jobs,
                                    .sampler = &sampler,
                                    .cache = &cache,
                                    .clipmap = {.levels = 4, .halfExtentTiles = 4},
                                    .centerX = 100,
                                    .centerZ = -300,
                                    .radius = 2000};
    {
        LodPremapper premapper(desc);
        const auto tiles = premapper.tiles();
        REQUIRE(!tiles.empty());
        CHECK(tiles.size() < 64 + (3 * 48)); // The radius leaves out the far corners of level 4.
        // Nearest first: the tile under the center leads.
        CHECK(tiles.front() == voxel::lodTileAt(1, 100, -300));
        runToEnd(premapper);
        const LodPremapper::Progress progress = premapper.progress();
        CHECK(progress.done == tiles.size());
        CHECK(progress.skipped == 0);
        CHECK(progress.failed == 0);
        CHECK(progress.fraction() == doctest::Approx(1.0));
        CHECK(sampler.calls.load() == tiles.size());
        // Every tile is in the cache under the manager's hash.
        const u64 hash = voxel::lodCacheHash(sampler.worldHash(), desc.options);
        for (const LodTileKey& key : tiles) {
            CHECK(cache.contains(key, hash));
        }
        CHECK_FALSE(cache.contains(tiles.front(), hash + 1));
    }
    // A second premap finds everything cached.
    LodPremapper again(desc);
    runToEnd(again);
    CHECK(again.progress().skipped == again.progress().total);
    CHECK(sampler.calls.load() == again.progress().total);
}

TEST_CASE("A cancelled premap stops early and leaves the near tiles baked") {
    const auto folder = freshFolder("cancel");
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 2, .ioThreadCount = 1}};
    CountingSampler sampler;
    sampler.slow.store(true);
    voxel::LodTileCache cache({.folder = folder});
    LodPremapper premapper({.jobs = &jobs,
                            .sampler = &sampler,
                            .cache = &cache,
                            .clipmap = {.levels = 6},
                            .radius = 100000,
                            .maxJobsInFlight = 4});
    const Stopwatch clock;
    while (premapper.progress().done < 4 && clock.elapsedMilliseconds() < 20000.0) {
        premapper.update();
        std::this_thread::yield();
    }
    premapper.cancel();
    runToEnd(premapper);
    const LodPremapper::Progress progress = premapper.progress();
    CHECK(progress.done >= 4);
    CHECK(progress.done < progress.total);
    CHECK(progress.failed == 0);
    // What was baked is the nearest.
    const u64 hash = voxel::lodCacheHash(sampler.worldHash(), {});
    CHECK(cache.contains(premapper.tiles().front(), hash));
    CHECK_FALSE(cache.contains(premapper.tiles().back(), hash));
}

TEST_CASE("A LodManager starting on a premapped area loads every tile from the cache") {
    const auto folder = freshFolder("manager");
    config::SettingsRegistry settings;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 4, .ioThreadCount = 2}};
    CountingSampler sampler;
    voxel::LodTileCache cache({.folder = folder});
    {
        LodPremapper premapper({.jobs = &jobs,
                                .sampler = &sampler,
                                .cache = &cache,
                                .clipmap = {},
                                .centerX = 40,
                                .centerZ = 40,
                                .radius = 1 << 20});
        runToEnd(premapper);
        CHECK(premapper.progress().done == 304);
    }
    const u32 baked = sampler.calls.load();
    voxel::LodManager manager({.jobs = &jobs, .settings = &settings, .sampler = &sampler, .cache = &cache});
    const Stopwatch clock;
    do {
        manager.update(DVec3(40.0, 80.0, 40.0));
        std::this_thread::yield();
    } while (!manager.isSettled() && clock.elapsedMilliseconds() < 20000.0);
    REQUIRE(manager.isSettled());
    CHECK(manager.counts().ready == 304);
    CHECK(sampler.calls.load() == baked); // Nothing sampled again.
    MESSAGE("304 premapped tiles ready from the cache in " << clock.elapsedMilliseconds() << " ms");
}
