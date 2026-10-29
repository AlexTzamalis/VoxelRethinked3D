// Light benchmarks (E10.2, E10.3): one chunk's sky and block light from its
// 48^3 neighborhood of terrain sample chunks.

#include "benchmarks.hpp"
#include "voxel/terrain_sample.hpp"

#include <vxe/voxel/chunk_lighter.hpp>

#include <nanobench.h>

#include <array>
#include <memory>

namespace vxe::bench {
namespace {

using voxel::BlockStateId;

// The sample's states: air and water clear or dimming, the rest opaque; one
// state glows so the block light flood has work.
std::shared_ptr<voxel::BlockRegistryView> sampleBlocks() {
    voxel::BlockRegistryBuilder builder;
    for (BlockStateId state = 1; state < 16; ++state) {
        voxel::BlockRenderInfo info{.layer = voxel::RenderLayer::Opaque};
        if (state == test::terrain::kWater) {
            info = {.layer = voxel::RenderLayer::Translucent, .lightOpacity = 3};
        }
        static_cast<void>(builder.add(info));
    }
    return builder.build();
}

} // namespace

void runLightBenchmarks() {
    const auto blocks = sampleBlocks();
    const ChunkPos surface = test::sampleChunkWithBits(4);
    std::array<voxel::ChunkStorage, 27> chunks;
    voxel::ChunkNeighborhood around{};
    for (usize k = 0; k < chunks.size(); ++k) {
        std::array<BlockStateId, kChunkVolume> states{};
        test::sampleChunk(surface.offset(static_cast<i32>(k % 3) - 1, static_cast<i32>(k / 9) - 1,
                                         static_cast<i32>((k / 3) % 3) - 1),
                          states);
        chunks.at(k).assign(states);
        around.at(k) = &chunks.at(k);
    }
    voxel::SkyOpenings open;
    open.set();
    voxel::ChunkLighter lighter;
    std::array<u8, kChunkVolume> light{};
    ankerl::nanobench::Bench bench;
    bench.title("ChunkLighter::compute (terrain surface neighborhood, open sky)").unit("chunk");
    bench.run("sky and block light", [&] {
        lighter.compute(around, *blocks, open, true, light);
        ankerl::nanobench::doNotOptimizeAway(light);
    });
    bench.run("sky light only", [&] {
        lighter.compute(around, *blocks, open, false, light);
        ankerl::nanobench::doNotOptimizeAway(light);
    });
}

} // namespace vxe::bench
