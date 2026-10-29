#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/block_light.hpp>
#include <vxe/voxel/streaming.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <queue>
#include <random>
#include <thread>
#include <utility>
#include <vector>

using namespace vxe;
using voxel::BlockStateId;

namespace {

constexpr BlockStateId kStone = 1;     // Opaque, blocks light.
constexpr BlockStateId kGlass = 2;     // Clear.
constexpr BlockStateId kWater = 3;     // Costs 3 levels per block.
constexpr BlockStateId kTorch = 4;     // Emits 14, clear.
constexpr BlockStateId kGlowstone = 5; // Emits 15, opaque.

std::shared_ptr<voxel::BlockRegistryView> lightBlocks() {
    voxel::BlockRegistryBuilder builder;
    const auto add = [&](voxel::RenderLayer layer, u8 opacity, u8 emitted) {
        voxel::BlockRenderInfo info{.layer = layer, .emittedLight = emitted, .lightOpacity = opacity};
        info.faceTextureLayer.fill(1);
        REQUIRE(builder.add(info));
    };
    add(voxel::RenderLayer::Opaque, 15, 0);
    add(voxel::RenderLayer::Translucent, 0, 0);
    add(voxel::RenderLayer::Translucent, 3, 0);
    add(voxel::RenderLayer::Cutout, 0, 14);
    add(voxel::RenderLayer::Opaque, 15, 15);
    return builder.build();
}

// Block light the slow, obvious way: relax every cell from its neighbors
// until nothing changes. `state(x, y, z)` gives the block anywhere in the
// box of `size` blocks per side; cells outside the box are dark air.
template <typename StateAt>
std::vector<u8> bruteForceLight(i32 size, const voxel::BlockRegistryView& blocks, const StateAt& state) {
    const auto at = [size](i32 x, i32 y, i32 z) {
        const auto side = static_cast<usize>(size);
        return (((static_cast<usize>(y) * side) + static_cast<usize>(z)) * side) + static_cast<usize>(x);
    };
    const auto side = static_cast<usize>(size);
    std::vector<u8> level(side * side * side, 0);
    for (i32 y = 0; y < size; ++y) {
        for (i32 z = 0; z < size; ++z) {
            for (i32 x = 0; x < size; ++x) {
                level[at(x, y, z)] = blocks.info(state(x, y, z)).emittedLight;
            }
        }
    }
    // The most light any of the 6 neighbors gives a cell costing `cost`.
    const auto brightestFrom = [&](i32 x, i32 y, i32 z, u8 cost) {
        u8 best = 0;
        const std::array<std::array<i32, 3>, 6> steps = {
            {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        for (const auto& step : steps) {
            const i32 nx = x + step[0];
            const i32 ny = y + step[1];
            const i32 nz = z + step[2];
            if (nx >= 0 && ny >= 0 && nz >= 0 && nx < size && ny < size && nz < size &&
                level[at(nx, ny, nz)] > cost) {
                best = std::max<u8>(best, static_cast<u8>(level[at(nx, ny, nz)] - cost));
            }
        }
        return best;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (i32 y = 0; y < size; ++y) {
            for (i32 z = 0; z < size; ++z) {
                for (i32 x = 0; x < size; ++x) {
                    const u8 opacity = blocks.info(state(x, y, z)).lightOpacity;
                    const u8 cost = std::max<u8>(opacity, 1);
                    if (cost >= 15) {
                        continue; // Opaque: only its own glow.
                    }
                    const u8 best = std::max(level[at(x, y, z)], brightestFrom(x, y, z, cost));
                    if (best != level[at(x, y, z)]) {
                        level[at(x, y, z)] = best;
                        changed = true;
                    }
                }
            }
        }
    }
    return level;
}

// A random block for a neighborhood: mostly air, some stone, a little
// glass and water, rare light sources. `roll` is 0 to 999.
BlockStateId randomBlock(u32 roll) {
    constexpr std::array<std::pair<u32, BlockStateId>, 5> kBelow = {
        {{150, kStone}, {170, kGlass}, {200, kWater}, {203, kTorch}, {204, kGlowstone}}};
    for (const auto& [limit, state] : kBelow) {
        if (roll < limit) {
            return state;
        }
    }
    return voxel::kAirState;
}

// Blocks for the streaming tests: a stone floor below layer 0; in layer 0 a
// few torches and glowstone (some right on chunk borders), stone pillars and
// a glass and water patch. Deterministic per position.
class LightSource final : public voxel::ChunkSource {
public:
    void produce(const voxel::ChunkRequest& request, voxel::ChunkProduct& out) override {
        const ChunkPos position = request.position();
        if (position.y < 0) {
            out.blocks.fill(kStone);
            return;
        }
        if (position.y > 0) {
            out.blocks.fill(voxel::kAirState);
            return;
        }
        std::array<BlockStateId, kChunkVolume> states{};
        const u64 seed =
            hashCombine(hashCombine(static_cast<u64>(position.x), static_cast<u64>(position.z)), 77);
        std::mt19937 random(
            static_cast<u32>(seed)); // NOLINT(bugprone-random-generator-seed): per chunk on purpose.
        for (u16 i = 0; i < 40; ++i) {
            const u16 at = static_cast<u16>(random() % kChunkVolume);
            states.at(at) = static_cast<BlockStateId>(1 + (random() % 3)); // Stone, glass or water.
        }
        // Light sources, two of them on the chunk's corner edges.
        states.at(LocalPos{.x = 15, .y = 3, .z = 0}.index()) = kTorch;
        states.at(LocalPos{.x = 0, .y = 15, .z = 15}.index()) = kGlowstone;
        states.at(LocalPos{.x = static_cast<u8>(random() % 16), .y = 7, .z = static_cast<u8>(random() % 16)}
                      .index()) = kTorch;
        out.blocks.assign(states);
    }
};

struct LightRig {
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    voxel::BlockRegistry blocks;
    voxel::VoxelWorld world;
    LightSource source;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 4, .ioThreadCount = 1}};
    std::unique_ptr<voxel::StreamingManager> manager;

    LightRig() {
        blocks.publish(lightBlocks());
        manager = std::make_unique<voxel::StreamingManager>(voxel::StreamingDesc{.world = &world,
                                                                                 .source = &source,
                                                                                 .jobs = &jobs,
                                                                                 .settings = &settings,
                                                                                 .blocks = &blocks,
                                                                                 .stats = &stats,
                                                                                 .minChunkY = -1,
                                                                                 .maxChunkY = 1});
        REQUIRE(settings.set("World.Streaming.RenderDistance", config::SettingValue{2}).hasValue());
        REQUIRE(settings.set("World.Streaming.VerticalRenderDistance", config::SettingValue{1}).hasValue());
    }
    ~LightRig() { manager.reset(); }
    LightRig(const LightRig&) = delete;
    LightRig& operator=(const LightRig&) = delete;
    LightRig(LightRig&&) = delete;
    LightRig& operator=(LightRig&&) = delete;

    void settle() const {
        const Stopwatch clock;
        do {
            manager->update({.position = DVec3(8.0, 8.0, 8.0)});
            std::this_thread::yield();
        } while (!manager->isSettled() && clock.elapsedMilliseconds() < 20000.0);
        REQUIRE(manager->isSettled());
    }

    // Every lit chunk's block light against a flood over the whole loaded
    // world (which holds every source within reach of the lit chunks),
    // written the plain way: a priority queue over every block, no chunks.
    void checkAgainstBruteForce() {
        constexpr i32 kReach = 4; // Loaded chunks: -4 to 4 sideways, layers -1 to 1.
        constexpr i32 kSide = (2 * kReach + 1) * kChunkSize;
        constexpr i32 kHeight = 3 * kChunkSize;
        const auto index = [](i32 x, i32 y, i32 z) {
            constexpr auto kSideCells = static_cast<usize>(kSide);
            return (((static_cast<usize>(y) * kSideCells) + static_cast<usize>(z)) * kSideCells) +
                   static_cast<usize>(x);
        };
        const voxel::BlockRegistryView& view = *blocks.current();
        std::vector<u8> level(static_cast<usize>(kSide) * kSide * kHeight, 0);
        std::vector<u8> cost(level.size(), 1);
        std::priority_queue<std::pair<u8, usize>> queue;
        for (i32 y = 0; y < kHeight; ++y) {
            for (i32 z = 0; z < kSide; ++z) {
                for (i32 x = 0; x < kSide; ++x) {
                    const BlockStateId state = world.getBlock({.x = x - (kReach * kChunkSize),
                                                               .y = y - kChunkSize,
                                                               .z = z - (kReach * kChunkSize)});
                    const voxel::BlockRenderInfo& info = view.info(state);
                    cost[index(x, y, z)] = std::max<u8>(info.lightOpacity, 1);
                    if (info.emittedLight > 0) {
                        level[index(x, y, z)] = info.emittedLight;
                        queue.emplace(info.emittedLight, index(x, y, z));
                    }
                }
            }
        }
        while (!queue.empty()) {
            const auto [value, cell] = queue.top();
            queue.pop();
            if (level[cell] != value) {
                continue;
            }
            const auto x = static_cast<i32>(cell % kSide);
            const auto z = static_cast<i32>((cell / kSide) % kSide);
            const auto y = static_cast<i32>(cell / (static_cast<usize>(kSide) * kSide));
            const std::array<std::array<i32, 3>, 6> steps = {
                {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
            for (const auto& step : steps) {
                const i32 nx = x + step[0];
                const i32 ny = y + step[1];
                const i32 nz = z + step[2];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= kSide || ny >= kHeight || nz >= kSide) {
                    continue;
                }
                const usize next = index(nx, ny, nz);
                if (cost[next] >= 15 || cost[next] >= value) {
                    continue;
                }
                const auto reached = static_cast<u8>(value - cost[next]);
                if (reached > level[next]) {
                    level[next] = reached;
                    queue.emplace(reached, next);
                }
            }
        }
        usize checked = 0;
        world.forEachChunk([&](const voxel::Chunk& chunk) {
            if (!manager->isLit(chunk.position())) {
                return;
            }
            const ChunkPos p = chunk.position();
            for (u16 i = 0; i < kChunkVolume; ++i) {
                const LocalPos local = LocalPos::fromIndex(i);
                const u8 want =
                    level[index(((p.x + kReach) * kChunkSize) + local.x, ((p.y + 1) * kChunkSize) + local.y,
                                ((p.z + kReach) * kChunkSize) + local.z)];
                const u8 got = voxel::blockLightOf(chunk.light().getIndex(i));
                if (got != want) {
                    FAIL_CHECK("Block light " << int(got) << " but expected " << int(want) << " at chunk "
                                              << p << " local " << int(local.x) << " " << int(local.y) << " "
                                              << int(local.z));
                    return;
                }
                ++checked;
            }
        });
        CHECK(checked >= usize{7} * 7 * 3 * kChunkVolume);
    }
};

} // namespace

TEST_CASE("Block light of a chunk matches a brute force flood over its neighborhood") {
    const auto blocks = lightBlocks();
    std::mt19937 random(31); // NOLINT(bugprone-random-generator-seed): the same worlds every run.
    voxel::BlockLighter lighter;
    for (i32 round = 0; round < 4; ++round) {
        std::array<voxel::ChunkStorage, 27> chunks;
        voxel::ChunkNeighborhood around{};
        for (usize k = 0; k < chunks.size(); ++k) {
            if (random() % 9 == 0 && k != 13) {
                continue; // Missing neighbor: dark open air.
            }
            std::array<BlockStateId, kChunkVolume> states{};
            for (BlockStateId& state : states) {
                state = randomBlock(random() % 1000);
            }
            chunks.at(k).assign(states);
            around.at(k) = &chunks.at(k);
        }
        CHECK(voxel::neighborhoodEmitsLight(around, *blocks));
        std::array<u8, kChunkVolume> light{};
        lighter.compute(around, *blocks, light);
        const auto stateAt = [&around](i32 x, i32 y, i32 z) {
            const auto k = (static_cast<usize>(y / 16) * 9) + (static_cast<usize>(z / 16) * 3) +
                           static_cast<usize>(x / 16);
            const voxel::ChunkStorage* chunk = around.at(k);
            return chunk == nullptr ? voxel::kAirState
                                    : chunk->get({.x = static_cast<u8>(x % 16),
                                                  .y = static_cast<u8>(y % 16),
                                                  .z = static_cast<u8>(z % 16)});
        };
        const std::vector<u8> expected = bruteForceLight(48, *blocks, stateAt);
        usize lit = 0;
        for (u16 i = 0; i < kChunkVolume; ++i) {
            const LocalPos local = LocalPos::fromIndex(i);
            const u8 want =
                expected[(((usize{16} + local.y) * 48 + (usize{16} + local.z)) * 48) + 16 + local.x];
            REQUIRE(light.at(i) == want);
            lit += want > 0 ? 1 : 0;
        }
        CHECK(lit > 0);
    }
    // Nothing that glows: nothing to compute.
    std::array<voxel::ChunkStorage, 27> stone;
    voxel::ChunkNeighborhood around{};
    for (usize k = 0; k < stone.size(); ++k) {
        stone.at(k).fill(kStone);
        around.at(k) = &stone.at(k);
    }
    CHECK_FALSE(voxel::neighborhoodEmitsLight(around, *blocks));
}

TEST_CASE("Streamed chunks are lit without seams across chunk borders") {
    LightRig rig;
    rig.settle();
    rig.checkAgainstBruteForce();
    rig.stats.endFrame(1.0 / 60.0);
    // Chunks within render distance + 1 are lit, the outer ring is not.
    CHECK(rig.manager->isLit({.x = 3, .y = 0, .z = 0}));
    CHECK_FALSE(rig.manager->isLit({.x = 4, .y = 0, .z = 0}));
}

TEST_CASE("Placing and removing a torch relights within a frame and leaves nothing behind") {
    LightRig rig;
    rig.settle();
    // A dark spot in the open air above the floor, far from any light source.
    const BlockPos torch{.x = 40, .y = 24, .z = 8};
    REQUIRE(rig.world.getBlock(torch) == voxel::kAirState);
    REQUIRE(voxel::blockLightOf(rig.world.getLight(torch)) == 0);

    const Stopwatch placed;
    REQUIRE(rig.world.setBlock(torch, kTorch));
    while (voxel::blockLightOf(rig.world.getLight(torch)) != 14 && placed.elapsedMilliseconds() < 5000.0) {
        rig.manager->update({.position = DVec3(8.0, 8.0, 8.0)});
    }
    const f64 placeMs = placed.elapsedMilliseconds();
    CHECK(voxel::blockLightOf(rig.world.getLight(torch)) == 14);
    rig.settle();
    rig.checkAgainstBruteForce();
    CHECK(voxel::blockLightOf(rig.world.getLight(torch.offset(5, 0, 0))) == 9);

    const Stopwatch removed;
    REQUIRE(rig.world.setBlock(torch, voxel::kAirState));
    while (voxel::blockLightOf(rig.world.getLight(torch)) != 0 && removed.elapsedMilliseconds() < 5000.0) {
        rig.manager->update({.position = DVec3(8.0, 8.0, 8.0)});
    }
    const f64 removeMs = removed.elapsedMilliseconds();
    rig.settle();
    rig.checkAgainstBruteForce(); // No light left where the torch was.
    CHECK(voxel::blockLightOf(rig.world.getLight(torch.offset(5, 0, 0))) == 0);
    MESSAGE("Torch: lit after " << placeMs << " ms, dark again after " << removeMs << " ms");
#if defined(NDEBUG) && !defined(VXE_SANITIZER_ACTIVE)
    if (test::timingIsReliable()) {
        CHECK(placeMs < 16.7);
        CHECK(removeMs < 16.7);
    }
#endif
}

TEST_CASE("Turning block light off removes it, turning it on brings it back") {
    LightRig rig;
    rig.settle();
    const auto anyBlockLight = [&rig] {
        bool any = false;
        rig.world.forEachChunk([&any](const voxel::Chunk& chunk) {
            for (u16 i = 0; i < kChunkVolume && !any; ++i) {
                any = voxel::blockLightOf(chunk.light().getIndex(i)) > 0;
            }
        });
        return any;
    };
    CHECK(anyBlockLight());
    REQUIRE(rig.settings.set("Graphics.Lighting.BlockLight", config::SettingValue{false}).hasValue());
    rig.settle();
    CHECK_FALSE(anyBlockLight());
    REQUIRE(rig.settings.set("Graphics.Lighting.BlockLight", config::SettingValue{true}).hasValue());
    rig.settle();
    rig.checkAgainstBruteForce();
}
