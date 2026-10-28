// Meshing benchmarks (E9.4, Section 10.7): all air, all stone, random 50%,
// a worst case checkerboard and the synthetic terrain sample, with the
// average time per chunk over the whole sample against the 0.06 ms budget.

#include "benchmarks.hpp"
#include "voxel/terrain_sample.hpp"

#include <vxe/core/time.hpp>
#include <vxe/voxel/mesher.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <nanobench.h>

#include <array>
#include <cstdio>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace vxe::bench {
namespace {

using voxel::BlockRenderInfo;
using voxel::BlockStateId;
using voxel::RenderLayer;

// Render info for the terrain sample's states (terrain_sample.hpp): water is
// translucent, everything else but air opaque, one texture per state.
std::shared_ptr<const voxel::BlockRegistryView> sampleBlocks(voxel::BlockRegistry& registry) {
    voxel::BlockRegistryBuilder builder;
    for (u16 state = 1; state <= test::terrain::kGravel; ++state) {
        BlockRenderInfo info{.layer = state == test::terrain::kWater ? RenderLayer::Translucent
                                                                     : RenderLayer::Opaque};
        info.faceTextureLayer.fill(state);
        static_cast<void>(builder.add(info));
    }
    registry.publish(builder.build());
    return registry.current();
}

voxel::ChunkStorage storageOf(ChunkPos position) {
    std::array<BlockStateId, kChunkVolume> states{};
    test::sampleChunk(position, states);
    voxel::ChunkStorage storage;
    storage.assign(states);
    return storage;
}

} // namespace

void runMeshingBenchmarks() {
    voxel::BlockRegistry registry;
    const auto blocks = sampleBlocks(registry);
    voxel::Mesher mesher;
    voxel::ChunkMesh mesh;
    const voxel::MeshOptions options;
    auto snapshot = std::make_unique<voxel::ChunkSnapshot>();

    // The whole terrain sample: build snapshots once, then mesh every chunk.
    voxel::VoxelWorld world;
    for (i32 cz = 0; cz < 8; ++cz) {
        for (i32 cx = 0; cx < 8; ++cx) {
            for (i32 cy = -4; cy < 8; ++cy) {
                world.insertChunk({.x = cx, .y = cy, .z = cz}, storageOf({.x = cx, .y = cy, .z = cz}));
            }
        }
    }
    std::vector<std::unique_ptr<voxel::ChunkSnapshot>> snapshots;
    world.forEachChunk([&](const voxel::Chunk& chunk) {
        auto copy = std::make_unique<voxel::ChunkSnapshot>();
        static_cast<void>(world.createSnapshot(chunk.position(), *copy));
        snapshots.push_back(std::move(copy));
    });
    usize quads = 0;
    usize meshedChunks = 0;
    f64 bestAverage = 1e9;
    for (int run = 0; run < 20; ++run) {
        quads = 0;
        meshedChunks = 0;
        const Stopwatch stopwatch;
        for (const auto& chunk : snapshots) {
            mesher.mesh(*chunk, *blocks, options, mesh);
            quads += mesh.quads.size();
            meshedChunks += mesh.empty() ? 0U : 1U;
        }
        bestAverage =
            std::min(bestAverage, stopwatch.elapsedMilliseconds() / static_cast<f64>(snapshots.size()));
    }
    const std::string summary = std::format(
        "\nTerrain sample meshing: {} chunks ({} with quads), average {:.4f} ms per chunk (budget 0.06), "
        "{:.1f} quads per non empty chunk, {:.0f} bytes of quads per non empty chunk\n\n",
        snapshots.size(), meshedChunks, bestAverage, static_cast<f64>(quads) / static_cast<f64>(meshedChunks),
        8.0 * static_cast<f64>(quads) / static_cast<f64>(meshedChunks));
    std::fputs(summary.c_str(), stdout);

    ankerl::nanobench::Bench bench;
    bench.title("Mesher::mesh (one chunk)").unit("chunk");
    const auto run = [&](const std::string& name) {
        bench.run(name, [&] {
            mesher.mesh(*snapshot, *blocks, options, mesh);
            ankerl::nanobench::doNotOptimizeAway(mesh.quads.data());
        });
    };
    snapshot->states.fill(test::terrain::kAir);
    run("all air");
    snapshot->states.fill(test::terrain::kStone);
    run("all stone (stone neighbors)");
    std::mt19937 random(1); // NOLINT(bugprone-random-generator-seed): the same chunk every run.
    for (BlockStateId& state : snapshot->states) {
        state = (random() & 1U) != 0 ? test::terrain::kStone : test::terrain::kAir;
    }
    run("random 50% stone");
    for (i32 y = -1; y <= 16; ++y) {
        for (i32 z = -1; z <= 16; ++z) {
            for (i32 x = -1; x <= 16; ++x) {
                snapshot->states[voxel::ChunkSnapshot::index(x, y, z)] =
                    ((x + y + z) & 1) == 0 ? test::terrain::kStone : test::terrain::kAir;
            }
        }
    }
    run("checkerboard (worst case)");
    static_cast<void>(world.createSnapshot(test::sampleChunkWithBits(4), *snapshot));
    run("terrain surface chunk");
}

} // namespace vxe::bench
