#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/chunk_lighter.hpp>
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

// A box of blocks for the reference floods: its size, the light opacity of
// every cell and the light levels being computed. Cells outside the box are
// dark open air.
struct Box {
    i32 sizeX = 0;
    i32 sizeY = 0;
    i32 sizeZ = 0;
    std::vector<u8> opacity;
    std::vector<u8> level;

    Box(i32 x, i32 y, i32 z)
        : sizeX(x)
        , sizeY(y)
        , sizeZ(z)
        , opacity(static_cast<usize>(x) * static_cast<usize>(y) * static_cast<usize>(z), 0)
        , level(opacity.size(), 0) {}

    [[nodiscard]] usize at(i32 x, i32 y, i32 z) const {
        return (((static_cast<usize>(y) * static_cast<usize>(sizeZ)) + static_cast<usize>(z)) *
                static_cast<usize>(sizeX)) +
               static_cast<usize>(x);
    }
    [[nodiscard]] bool inside(i32 x, i32 y, i32 z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < sizeX && y < sizeY && z < sizeZ;
    }

    // Full sky light down every open column until something dims or stops it.
    template <typename Open>
    void seedSky(const Open& open) {
        for (i32 z = 0; z < sizeZ; ++z) {
            for (i32 x = 0; x < sizeX; ++x) {
                if (!open(x, z)) {
                    continue;
                }
                for (i32 y = sizeY - 1; y >= 0; --y) {
                    const u8 cost = opacity[at(x, y, z)];
                    if (cost >= 15) {
                        break;
                    }
                    level[at(x, y, z)] = std::max<u8>(level[at(x, y, z)], static_cast<u8>(15 - cost));
                    if (cost > 0) {
                        break;
                    }
                }
            }
        }
    }

    // The most light a cell gets from its 6 neighbors.
    [[nodiscard]] u8 brightestFrom(i32 x, i32 y, i32 z) const {
        const u8 cost = std::max<u8>(opacity[at(x, y, z)], 1);
        u8 best = 0;
        constexpr std::array<std::array<i32, 3>, 6> kSteps = {
            {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        for (const auto& step : kSteps) {
            const i32 nx = x + step[0];
            const i32 ny = y + step[1];
            const i32 nz = z + step[2];
            if (inside(nx, ny, nz) && level[at(nx, ny, nz)] > cost) {
                best = std::max<u8>(best, static_cast<u8>(level[at(nx, ny, nz)] - cost));
            }
        }
        return best;
    }

    // The slow, obvious spread: relax every cell until nothing changes.
    void relax() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (i32 y = 0; y < sizeY; ++y) {
                for (i32 z = 0; z < sizeZ; ++z) {
                    for (i32 x = 0; x < sizeX; ++x) {
                        if (opacity[at(x, y, z)] >= 15) {
                            continue; // Opaque: only its own glow.
                        }
                        const u8 best = std::max(level[at(x, y, z)], brightestFrom(x, y, z));
                        if (best != level[at(x, y, z)]) {
                            level[at(x, y, z)] = best;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    // The same spread with a priority queue, for boxes too big to relax.
    void flood() {
        std::priority_queue<std::pair<u8, usize>> queue;
        for (usize cell = 0; cell < level.size(); ++cell) {
            if (level[cell] > 0) {
                queue.emplace(level[cell], cell);
            }
        }
        const auto planeCells = static_cast<usize>(sizeX) * static_cast<usize>(sizeZ);
        while (!queue.empty()) {
            const auto [value, cell] = queue.top();
            queue.pop();
            if (level[cell] != value) {
                continue;
            }
            const auto x = static_cast<i32>(cell % static_cast<usize>(sizeX));
            const auto z = static_cast<i32>((cell / static_cast<usize>(sizeX)) % static_cast<usize>(sizeZ));
            const auto y = static_cast<i32>(cell / planeCells);
            constexpr std::array<std::array<i32, 3>, 6> kSteps = {
                {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
            for (const auto& step : kSteps) {
                if (!inside(x + step[0], y + step[1], z + step[2])) {
                    continue;
                }
                const usize next = at(x + step[0], y + step[1], z + step[2]);
                const u8 cost = std::max<u8>(opacity[next], 1);
                if (cost >= 15 || cost >= value || level[next] >= value - cost) {
                    continue;
                }
                level[next] = static_cast<u8>(value - cost);
                queue.emplace(level[next], next);
            }
        }
    }
};

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
// few torches and glowstone (some right on chunk borders) and scattered
// stone, glass and water under open sky; air above. Deterministic per position.
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
        // NOLINTNEXTLINE(bugprone-random-generator-seed): per chunk on purpose.
        std::mt19937 random(static_cast<u32>(seed));
        for (u16 i = 0; i < 40; ++i) {
            const u16 at = static_cast<u16>(random() % kChunkVolume);
            states.at(at) = static_cast<BlockStateId>(1 + (random() % 3)); // Stone, glass or water.
        }
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

    void update() const { manager->update({.position = DVec3(8.0, 8.0, 8.0)}); }

    void settle() const {
        const Stopwatch clock;
        do {
            update();
            std::this_thread::yield();
        } while (!manager->isSettled() && clock.elapsedMilliseconds() < 20000.0);
        REQUIRE(manager->isSettled());
    }

    // Every lit chunk's sky and block light against floods over the whole
    // loaded world, written the plain way (no chunks): it holds every source
    // within reach of the lit chunks, and the open sky above the world's top.
    void checkAgainstBruteForce() {
        constexpr i32 kReach = 4; // Loaded chunks: -4 to 4 sideways, layers -1 to 1.
        constexpr i32 kSide = (2 * kReach + 1) * kChunkSize;
        const voxel::BlockRegistryView& view = *blocks.current();
        Box sky(kSide, 3 * kChunkSize, kSide);
        Box block(kSide, 3 * kChunkSize, kSide);
        for (i32 y = 0; y < sky.sizeY; ++y) {
            for (i32 z = 0; z < kSide; ++z) {
                for (i32 x = 0; x < kSide; ++x) {
                    const voxel::BlockRenderInfo& info =
                        view.info(world.getBlock({.x = x - (kReach * kChunkSize),
                                                  .y = y - kChunkSize,
                                                  .z = z - (kReach * kChunkSize)}));
                    sky.opacity[sky.at(x, y, z)] = info.lightOpacity;
                    block.opacity[block.at(x, y, z)] = info.lightOpacity;
                    block.level[block.at(x, y, z)] = info.emittedLight;
                }
            }
        }
        sky.seedSky([](i32, i32) { return true; });
        sky.flood();
        block.flood();
        usize checked = 0;
        world.forEachChunk([&](const voxel::Chunk& chunk) {
            if (!manager->isLit(chunk.position())) {
                return;
            }
            const ChunkPos p = chunk.position();
            for (u16 i = 0; i < kChunkVolume; ++i) {
                const LocalPos local = LocalPos::fromIndex(i);
                const usize cell =
                    sky.at(((p.x + kReach) * kChunkSize) + local.x, ((p.y + 1) * kChunkSize) + local.y,
                           ((p.z + kReach) * kChunkSize) + local.z);
                const u8 light = chunk.light().getIndex(i);
                if (voxel::skyLightOf(light) != sky.level[cell] ||
                    voxel::blockLightOf(light) != block.level[cell]) {
                    FAIL_CHECK("Light sky " << int(voxel::skyLightOf(light)) << " block "
                                            << int(voxel::blockLightOf(light)) << ", expected sky "
                                            << int(sky.level[cell]) << " block " << int(block.level[cell])
                                            << " at chunk " << p << " local " << int(local.x) << " "
                                            << int(local.y) << " " << int(local.z));
                    return;
                }
                ++checked;
            }
        });
        CHECK(checked >= usize{7} * 7 * 3 * kChunkVolume);
    }
};

} // namespace

TEST_CASE("Chunk light matches brute force floods over its neighborhood") {
    const auto blocks = lightBlocks();
    std::mt19937 random(31); // NOLINT(bugprone-random-generator-seed): the same worlds every run.
    voxel::ChunkLighter lighter;
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
        // Open sky over three quarters of the columns.
        voxel::SkyOpenings openings;
        for (usize column = 0; column < openings.size(); ++column) {
            openings.set(column, random() % 4 != 0);
        }
        CHECK(voxel::neighborhoodEmitsLight(around, *blocks));
        std::array<u8, kChunkVolume> light{};
        lighter.compute(around, *blocks, openings, true, light);

        Box sky(48, 48, 48);
        Box block(48, 48, 48);
        for (i32 y = 0; y < 48; ++y) {
            for (i32 z = 0; z < 48; ++z) {
                for (i32 x = 0; x < 48; ++x) {
                    const auto k = (static_cast<usize>(y / 16) * 9) + (static_cast<usize>(z / 16) * 3) +
                                   static_cast<usize>(x / 16);
                    const BlockStateId state = around.at(k) == nullptr
                                                   ? voxel::kAirState
                                                   : around.at(k)->get({.x = static_cast<u8>(x % 16),
                                                                        .y = static_cast<u8>(y % 16),
                                                                        .z = static_cast<u8>(z % 16)});
                    const voxel::BlockRenderInfo& info = blocks->info(state);
                    sky.opacity[sky.at(x, y, z)] = info.lightOpacity;
                    block.opacity[block.at(x, y, z)] = info.lightOpacity;
                    block.level[block.at(x, y, z)] = info.emittedLight;
                }
            }
        }
        sky.seedSky([&openings](i32 x, i32 z) {
            return openings.test((static_cast<usize>(z) * 48) + static_cast<usize>(x));
        });
        sky.relax();
        block.relax();
        usize skyLit = 0;
        for (u16 i = 0; i < kChunkVolume; ++i) {
            const LocalPos local = LocalPos::fromIndex(i);
            const usize cell = sky.at(16 + local.x, 16 + local.y, 16 + local.z);
            REQUIRE(voxel::skyLightOf(light.at(i)) == sky.level[cell]);
            REQUIRE(voxel::blockLightOf(light.at(i)) == block.level[cell]);
            skyLit += sky.level[cell] > 0 ? 1U : 0U;
        }
        CHECK(skyLit > 0);
    }
    // Nothing that glows: no light source to flood from.
    std::array<voxel::ChunkStorage, 27> stone;
    voxel::ChunkNeighborhood around{};
    for (usize k = 0; k < stone.size(); ++k) {
        stone.at(k).fill(kStone);
        around.at(k) = &stone.at(k);
    }
    CHECK_FALSE(voxel::neighborhoodEmitsLight(around, *blocks));
}

TEST_CASE("Streamed chunks are lit by sky and blocks without seams across chunk borders") {
    LightRig rig;
    rig.settle();
    rig.checkAgainstBruteForce();
    // Chunks within render distance + 1 are lit, the outer ring is not.
    CHECK(rig.manager->isLit({.x = 3, .y = 0, .z = 0}));
    CHECK_FALSE(rig.manager->isLit({.x = 4, .y = 0, .z = 0}));
    // Solid stone is dark; the air above the ground has full sky light (and
    // some block light from glowstone on the ground's top layer).
    CHECK(rig.world.findChunk({.x = 0, .y = -1, .z = 0})->light() == voxel::ChunkLight(voxel::kDarkLight));
    const voxel::ChunkLight& air = rig.world.findChunk({.x = 0, .y = 1, .z = 0})->light();
    for (u16 i = 0; i < kChunkVolume; ++i) {
        REQUIRE(voxel::skyLightOf(air.getIndex(i)) == 15);
    }
    rig.stats.endFrame(1.0 / 60.0);
    CHECK(rig.stats.find("Voxel.Light.Skipped").isValid());
}

TEST_CASE("A roof shades the ground below it, and taking it away lets the sky back in") {
    LightRig rig;
    rig.settle();
    // A 9 x 9 stone roof high up, over a spot of plain ground.
    const BlockPos ground{.x = 40, .y = 15, .z = 40};
    for (i64 dz = -4; dz <= 4; ++dz) {
        for (i64 dx = -4; dx <= 4; ++dx) {
            REQUIRE(rig.world.setBlock(ground.offset(dx, 15, dz), kStone));
        }
    }
    rig.settle();
    rig.checkAgainstBruteForce();
    const BlockPos under = ground.offset(0, 1, 0);
    CHECK(voxel::skyLightOf(rig.world.getLight(under)) < 15);
    for (i64 dz = -4; dz <= 4; ++dz) {
        for (i64 dx = -4; dx <= 4; ++dx) {
            REQUIRE(rig.world.setBlock(ground.offset(dx, 15, dz), voxel::kAirState));
        }
    }
    rig.settle();
    rig.checkAgainstBruteForce();
    CHECK(voxel::skyLightOf(rig.world.getLight(under)) == 15);
}

TEST_CASE("Placing and removing a torch relights within a frame and leaves nothing behind") {
    LightRig rig;
    rig.settle();
    // Inside a closed stone room, so no sky light reaches it.
    const BlockPos torch{.x = 40, .y = 24, .z = 8};
    for (i64 dz = -6; dz <= 6; ++dz) {
        for (i64 dy = -6; dy <= 6; ++dy) {
            for (i64 dx = -6; dx <= 6; ++dx) {
                const bool wall = std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) == 6;
                REQUIRE(rig.world.setBlock(torch.offset(dx, dy, dz), wall ? kStone : voxel::kAirState));
            }
        }
    }
    rig.settle();
    REQUIRE(rig.world.getLight(torch) == voxel::kDarkLight);

    const Stopwatch placed;
    REQUIRE(rig.world.setBlock(torch, kTorch));
    while (voxel::blockLightOf(rig.world.getLight(torch)) != 14 && placed.elapsedMilliseconds() < 5000.0) {
        rig.update();
    }
    const f64 placeMs = placed.elapsedMilliseconds();
    CHECK(voxel::blockLightOf(rig.world.getLight(torch)) == 14);
    rig.settle();
    rig.checkAgainstBruteForce();
    CHECK(voxel::blockLightOf(rig.world.getLight(torch.offset(5, 0, 0))) == 9);

    const Stopwatch removed;
    REQUIRE(rig.world.setBlock(torch, voxel::kAirState));
    while (voxel::blockLightOf(rig.world.getLight(torch)) != 0 && removed.elapsedMilliseconds() < 5000.0) {
        rig.update();
    }
    const f64 removeMs = removed.elapsedMilliseconds();
    rig.settle();
    rig.checkAgainstBruteForce(); // No light left where the torch was.
    CHECK(rig.world.getLight(torch.offset(5, 0, 0)) == voxel::kDarkLight);
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
