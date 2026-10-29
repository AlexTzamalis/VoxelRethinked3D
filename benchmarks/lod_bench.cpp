// LOD data benchmarks (E11.1): building a tile, the cache file codec, the
// cache on disk and clipmap updates.

#include "benchmarks.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/lod_cache.hpp>
#include <vxe/voxel/lod_manager.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <nanobench.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

namespace vxe::bench {
namespace {

using voxel::LodSpan;
using voxel::LodTileKey;

// Hills with an overhang in every tenth cell: about 1.1 spans per cell.
voxel::LodTile hillsTile(LodTileKey key) {
    voxel::LodTileBuilder builder(key);
    std::vector<LodSpan> spans;
    for (i32 cz = 0; cz < voxel::kLodTileCells; ++cz) {
        for (i32 cx = 0; cx < voxel::kLodTileCells; ++cx) {
            spans.clear();
            const auto ground =
                static_cast<i32>(64.0 + (20.0 * std::sin(cx * 0.1)) + (15.0 * std::cos(cz * 0.07)));
            if ((cx + (cz * 7)) % 10 == 0) {
                spans.push_back(
                    {.bottomY = ground + 30, .topY = ground + 34, .topMaterial = 7, .sideMaterial = 8});
            }
            spans.push_back({.bottomY = -1024, .topY = ground, .topMaterial = 1, .sideMaterial = 3});
            builder.addCell(spans);
        }
    }
    return std::move(builder).build();
}

// Flat ground, one span per cell: the cheapest possible sampler, so the
// manager's own costs show.
class FlatSampler final : public voxel::LodTileSampler {
public:
    bool sampleTile(const LodTileKey& /*key*/, const voxel::LodSampleOptions& /*options*/,
                    voxel::LodTileBuilder& builder, const std::atomic<bool>& /*cancelled*/) override {
        const LodSpan ground{.bottomY = -64, .topY = 64, .topMaterial = 1, .sideMaterial = 2};
        for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
            builder.addCell(std::span(&ground, 1));
        }
        return true;
    }
    [[nodiscard]] u64 worldHash() const override { return 1; }
};

// The manager on the main thread: one update when nothing changes (every
// frame), and the time to get all 304 tiles of the default clipmap ready.
void runManagerBenchmarks() {
    config::SettingsRegistry settings;
    FlatSampler sampler;
    jobs::JobSystem jobs{jobs::JobSystemConfig{}};
    voxel::LodManager manager({.jobs = &jobs, .settings = &settings, .sampler = &sampler});
    const Stopwatch settle;
    while (!manager.isSettled() || manager.counts().wanted == 0) {
        manager.update(DVec3(0.0));
        std::this_thread::yield();
    }
    const std::string line = std::format(
        "LodManager: 304 tiles ready {:.1f} ms after the first update ({} workers, flat sampler)\n",
        settle.elapsedMilliseconds(), jobs.workerCount());
    std::fputs(line.c_str(), stdout);
    ankerl::nanobench::Bench bench;
    bench.title("LodManager::update").unit("update");
    bench.run("settled, camera still", [&] { manager.update(DVec3(0.0)); });
}

} // namespace

void runLodBenchmarks() {
    runManagerBenchmarks();
    const LodTileKey key{.level = 3, .x = 5, .z = -2};
    const u64 world = voxel::lodWorldHash(1, 1, 0);
    const voxel::LodTile tile = hillsTile(key);
    std::vector<std::byte> bytes;
    voxel::encodeLodTile(tile, world, bytes);

    ankerl::nanobench::Bench bench;
    bench.title("LOD tiles (64 x 64 cells of hills, 1.1 spans per cell)").unit("tile");
    bench.run("build with LodTileBuilder", [&] {
        auto built = hillsTile(key);
        ankerl::nanobench::doNotOptimizeAway(built);
    });
    bench.run("encode (varints + LZ4)", [&] {
        bytes.clear();
        voxel::encodeLodTile(tile, world, bytes);
        ankerl::nanobench::doNotOptimizeAway(bytes);
    });
    bench.run("decode", [&] {
        auto decoded = voxel::decodeLodTile(bytes, world, key);
        ankerl::nanobench::doNotOptimizeAway(decoded);
    });

    const auto folder = std::filesystem::temp_directory_path() / "vxe_bench" / "lod";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    voxel::LodTileCache cache({.folder = folder, .worldHash = world});
    bench.run("cache save (atomic file write)", [&] { static_cast<void>(cache.save(tile)); });
    bench.run("cache load (file read and decode)", [&] {
        auto loaded = cache.load(key);
        ankerl::nanobench::doNotOptimizeAway(loaded);
    });
    std::filesystem::remove_all(folder, error);

    voxel::LodClipmap clipmap({.firstLevel = 1, .levels = 6, .halfExtentTiles = 4});
    i64 x = 0;
    ankerl::nanobench::Bench moves;
    moves.title("LodClipmap::update (6 levels, 304 tiles)").unit("update");
    moves.run("camera moves 1 block (no change)", [&] {
        ++x;
        ankerl::nanobench::doNotOptimizeAway(clipmap.update(x & 63, 0));
    });
    moves.run("camera moves 256 blocks (level 1 and 2 change)", [&] {
        x += 256;
        ankerl::nanobench::doNotOptimizeAway(clipmap.update(x, 0));
    });
}

} // namespace vxe::bench
