// Voxel world benchmarks (E9.3): snapshot creation with uniform, mixed and
// missing neighbors, and block edits through the world.

#include "benchmarks.hpp"
#include "voxel/terrain_sample.hpp"

#include <vxe/voxel/voxel_world.hpp>

#include <nanobench.h>

#include <array>
#include <memory>

namespace vxe::bench {
namespace {

using voxel::BlockStateId;
using voxel::ChunkStorage;

ChunkStorage sampleStorage(ChunkPos position) {
    std::array<BlockStateId, kChunkVolume> states{};
    test::sampleChunk(position, states);
    ChunkStorage storage;
    storage.assign(states);
    return storage;
}

// A terrain surface chunk at the origin with neighbors made by `neighbor`.
template <typename Fn>
void buildWorld(voxel::VoxelWorld& world, const Fn& neighbor) {
    world.insertChunk({}, sampleStorage(test::sampleChunkWithBits(4)));
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    world.insertChunk({.x = dx, .y = dy, .z = dz}, neighbor(dx, dy, dz));
                }
            }
        }
    }
}

} // namespace

void runWorldBenchmarks() {
    auto snapshot = std::make_unique<voxel::ChunkSnapshot>();
    ankerl::nanobench::Bench bench;
    bench.title("VoxelWorld::createSnapshot (18^3, 4 bit surface center)").unit("snapshot");

    voxel::VoxelWorld uniform;
    buildWorld(uniform,
               [](i32, i32 dy, i32) { return ChunkStorage(dy < 0 ? BlockStateId{1} : voxel::kAirState); });
    bench.run("uniform neighbors", [&] {
        uniform.createSnapshot({}, *snapshot);
        ankerl::nanobench::doNotOptimizeAway(snapshot->states);
    });

    voxel::VoxelWorld mixed;
    buildWorld(mixed, [](i32 dx, i32 dy, i32 dz) {
        return sampleStorage(test::sampleChunkWithBits(4).offset(dx, dy, dz));
    });
    bench.run("terrain neighbors (mixed)", [&] {
        mixed.createSnapshot({}, *snapshot);
        ankerl::nanobench::doNotOptimizeAway(snapshot->states);
    });

    voxel::VoxelWorld alone;
    alone.insertChunk({}, sampleStorage(test::sampleChunkWithBits(4)));
    bench.run("no neighbors loaded", [&] {
        alone.createSnapshot({}, *snapshot);
        ankerl::nanobench::doNotOptimizeAway(snapshot->states);
    });

    voxel::VoxelWorld sky;
    sky.insertChunk({});
    bench.run("all air, uniform neighbors", [&] {
        sky.createSnapshot({}, *snapshot);
        ankerl::nanobench::doNotOptimizeAway(snapshot->states);
    });

    ankerl::nanobench::Bench edits;
    edits.title("VoxelWorld::setBlock (interior, alternating states)").unit("edit").batch(2);
    BlockPos at{.x = 7, .y = 55, .z = 7};
    edits.run("surface chunk", [&] {
        mixed.setBlock(at, 1);
        mixed.setBlock(at, 3);
    });
}

} // namespace vxe::bench
