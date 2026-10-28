#include <vxe/core/stats.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace vxe;
using namespace vxe::voxel;

namespace {

BlockRenderInfo stone(u16 layer) {
    BlockRenderInfo info{.layer = RenderLayer::Opaque};
    info.faceTextureLayer.fill(layer);
    return info;
}

} // namespace

TEST_CASE("The builder starts with air and hands out ids in order") {
    BlockRegistryBuilder builder;
    CHECK(builder.size() == 1);
    const auto first = builder.add(stone(3));
    const auto torch = builder.add({.layer = RenderLayer::Cutout, .emittedLight = 14, .lightOpacity = 0});
    REQUIRE(first.hasValue());
    REQUIRE(torch.hasValue());
    CHECK(first.value() == 1);
    CHECK(torch.value() == 2);

    const auto view = builder.build();
    CHECK(view->size() == 3);
    CHECK(view->info(kAirState) == airRenderInfo());
    CHECK_FALSE(view->info(kAirState).isVisible());
    CHECK(view->info(kAirState).lightOpacity == 0);
    CHECK(view->info(1).isOpaque());
    CHECK(view->info(1).textureLayer(BlockFace::NegY) == 3);
    CHECK(view->info(2).emittedLight == 14);
    // Ids the view does not know (a newer save, a stale mesh job) read as air.
    CHECK(view->info(500) == airRenderInfo());
}

TEST_CASE("The builder rejects bad light values and running out of ids") {
    BlockRegistryBuilder builder;
    const auto tooBright = builder.add({.layer = RenderLayer::Opaque, .emittedLight = 16});
    REQUIRE_FALSE(tooBright.hasValue());
    CHECK(tooBright.error().code() == ErrorCode::InvalidArgument);
    CHECK(tooBright.error().message() ==
          "Block state 1: light values go from 0 to 15 (emitted 16, opacity 15)");
    CHECK_FALSE(builder.add({.lightOpacity = 20}).hasValue());
    CHECK(builder.size() == 1); // Failed adds change nothing.

    for (usize i = 1; i < kMaxBlockStates; ++i) {
        REQUIRE(builder.add(stone(0)).hasValue());
    }
    CHECK(builder.size() == kMaxBlockStates);
    const auto full = builder.add(stone(0));
    REQUIRE_FALSE(full.hasValue());
    CHECK(full.error().code() == ErrorCode::OutOfMemory);
    CHECK(builder.build()->info(65535).isOpaque());
}

TEST_CASE("Publishing swaps the view while old views stay valid") {
    stats::StatRegistry stats;
    BlockRegistry registry(&stats);
    const auto initial = registry.current();
    CHECK(initial->size() == 1); // Only air before anything is published.
    CHECK(initial->generation() == 1);

    BlockRegistryBuilder builder;
    REQUIRE(builder.add(stone(7)).hasValue());
    registry.publish(builder.build());
    const auto second = registry.current();
    CHECK(second->size() == 2);
    CHECK(second->generation() == 2);
    CHECK(registry.generation() == 2);
    // A job that took the first view still sees it unchanged.
    CHECK(initial->size() == 1);
    CHECK(initial->info(1) == airRenderInfo());
    stats.endFrame(1.0 / 60.0);
    CHECK(stats.value(stats.find("Voxel.BlockRegistry.States")) == 2.0);
}

// Readers on worker threads and a publisher racing them: every view a reader
// gets is complete and consistent. Run under the tsan preset in CI.
TEST_CASE("Views can be read on many threads while new ones are published") {
    BlockRegistry registry;
    std::atomic<bool> stop{false};
    std::atomic<u64> reads{0};
    std::atomic<u64> inconsistent{0};
    std::vector<std::jthread> readers;
    readers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto view = registry.current();
                // Every published view i has i + 1 states, state k using texture layer k.
                for (usize k = 1; k < view->size(); ++k) {
                    if (view->info(static_cast<BlockStateId>(k)).textureLayer(BlockFace::PosX) != k) {
                        inconsistent.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // Publish only once every reader is running, so reads and publishes overlap.
    while (reads.load(std::memory_order_relaxed) < 4) {
        std::this_thread::yield();
    }
    BlockRegistryBuilder builder;
    for (u16 k = 1; k <= 200; ++k) {
        REQUIRE(builder.add(stone(k)).hasValue());
        registry.publish(builder.build());
    }
    stop = true;
    readers.clear();
    CHECK(inconsistent.load() == 0);
    CHECK(reads.load() > 0);
    CHECK(registry.current()->size() == 201);
    CHECK(registry.generation() == 201);
}
