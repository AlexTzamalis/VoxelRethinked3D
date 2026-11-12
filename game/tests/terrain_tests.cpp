#include "worldgen/terrain.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace vr;
using vxe::f64;
using vxe::i32;
using vxe::i64;
using vxe::u64;
using vxe::usize;

namespace {

using States = std::array<vxe::voxel::BlockStateId, vxe::kChunkVolume>;

const TerrainBlocks& blocks() {
    static const TerrainBlocks kBlocks = [] {
        auto catalog = BlockCatalog::loadFolder(VR_BLOCKS_DIR);
        REQUIRE(catalog.hasValue());
        auto found = TerrainBlocks::fromCatalog(catalog.value());
        REQUIRE(found.hasValue());
        return found.value();
    }();
    return kBlocks;
}

} // namespace

TEST_CASE("Terrain gives the same chunk in any order, with any cache") {
    const Terrain terrain(blocks(), 11, PlanetPreset{}, false);
    const Terrain same(blocks(), 11, PlanetPreset{}, false);
    const vxe::BlockPos land = terrain.spawnColumn();
    const vxe::ChunkPos chunk = vxe::toChunk(land);
    auto first = std::make_unique<States>();
    auto second = std::make_unique<States>();
    terrain.generate(chunk, *first);
    // Pushes the column out of this thread's cache.
    for (i32 i = 0; i < 2000; ++i) {
        CHECK(same.surfaceHeight(i64{i} * 16, 0) < 4000);
    }
    same.generate(chunk, *second);
    CHECK(*first == *second);
}

TEST_CASE("Two terrains do not share cached columns") {
    const Terrain a(blocks(), 1, PlanetPreset{}, false);
    const Terrain b(blocks(), 2, PlanetPreset{}, false);
    usize different = 0;
    for (i64 i = 0; i < 100; ++i) {
        different += a.surfaceHeight(i * 900, 0) != b.surfaceHeight(i * 900, 0) ? 1U : 0U;
    }
    CHECK(different > 80);
}

TEST_CASE("Columns meet: chunk borders show no step the slope does not explain") {
    for (const u64 seed : {3ULL, 12345ULL}) {
        const Terrain terrain(blocks(), seed, PlanetPreset{}, false);
        i64 worstBorder = 0;
        i64 worstInside = 0;
        for (i64 z = 0; z < 800; z += 3) {
            i64 last = terrain.surfaceHeight(0, z);
            for (i64 x = 1; x < 800; ++x) {
                const i64 height = terrain.surfaceHeight(x, z);
                const i64 step = std::abs(height - last);
                (x % 16 == 0 ? worstBorder : worstInside) =
                    std::max(x % 16 == 0 ? worstBorder : worstInside, step);
                last = height;
            }
        }
        MESSAGE("seed " << seed << ": worst step " << worstBorder << " on chunk borders, " << worstInside
                        << " inside chunks");
        CHECK(worstBorder <= worstInside + 2);
    }
}

TEST_CASE("Gullies carve the land: slopes are rougher than the planet alone") {
    const Terrain terrain(blocks(), 2, PlanetPreset{}, false);
    usize changed = 0;
    usize steep = 0;
    f64 lowered = 0.0;
    for (i64 z = 0; z < 120; ++z) {
        for (i64 x = 0; x < 120; ++x) {
            const auto wx = static_cast<f64>(x * 170);
            const auto wz = static_cast<f64>(z * 170);
            const f64 plain =
                terrain.planet().sample(wx, wz).elevation + static_cast<f64>(Terrain::kSeaLevel);
            const f64 carved = terrain.farGround(wx, wz, 1.0).height;
            if (plain > 170.0) {
                ++steep;
                lowered += plain - carved;
                changed += std::abs(plain - carved) > 1.0 ? 1U : 0U;
            }
        }
    }
    REQUIRE(steep > 50);
    MESSAGE(steep << " high samples, " << changed << " changed, lowered " << lowered / static_cast<f64>(steep)
                  << " on average");
    CHECK(changed * 2 > steep);
    CHECK(lowered > 0.0); // Erosion takes ground away.
}

TEST_CASE("Far terrain stands on the same ground as chunks") {
    const Terrain terrain(blocks(), 5, PlanetPreset{}, false);
    f64 worst = 0.0;
    for (i64 i = 0; i < 2000; ++i) {
        const i64 x = i * 131;
        const i64 z = i * -77;
        const f64 far = terrain.farGround(static_cast<f64>(x), static_cast<f64>(z), 8.0).height;
        worst = std::max(worst, std::abs(far - static_cast<f64>(terrain.surfaceHeight(x, z))));
    }
    // Chunks blend heights 4 blocks apart and far terrain reads the point
    // itself, so on the steepest faces (8 blocks per block) they differ by a
    // part of that slope.
    MESSAGE("Worst gap between far terrain and chunks: " << worst);
    CHECK(worst < 24.0);
}

TEST_CASE("High above the ground a chunk is air, and the floor closes the world") {
    const Terrain terrain(blocks(), 6, PlanetPreset{}, false);
    auto states = std::make_unique<States>();
    terrain.generate({.x = 0, .y = 180, .z = 0}, *states);
    CHECK(std::ranges::all_of(*states,
                              [](vxe::voxel::BlockStateId state) { return state == vxe::voxel::kAirState; }));
    terrain.generate({.x = 0, .y = Terrain::kFloorY >> 4, .z = 0}, *states);
    CHECK((*states)[0] == blocks().floor);
}

TEST_CASE("Players start on dry land on every seed") {
    for (const u64 seed : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL, 12345ULL, 987654321ULL}) {
        const Terrain terrain(blocks(), seed, PlanetPreset{}, false);
        const vxe::BlockPos spawn = terrain.spawnColumn();
        CHECK(spawn.y > Terrain::kSeaLevel + 2);
        CHECK(spawn.y < Terrain::kSeaLevel + 90);
    }
}

TEST_CASE("Every biome has its look, and everything is placed somewhere") {
    const TerrainBlocks& ids = blocks();
    for (usize i = 0; i < kBiomeCount; ++i) {
        const BiomeLook& look = ids.biomes[i];
        CHECK(look.top != vxe::voxel::kAirState);
        CHECK(look.under != vxe::voxel::kAirState);
        CHECK(look.floor != vxe::voxel::kAirState);
    }
    // A long walk through one world finds every kind of tree. Each look at a
    // cell far from the last builds a whole column, so this stays a few
    // thousand looks, spread widely enough for rare biomes.
    const Terrain terrain(ids, 12345, PlanetPreset{}, false);
    usize trees = 0;
    std::array<usize, kTreeKindCount> kinds{};
    for (i64 z = -50; z < 50; ++z) {
        for (i64 x = -50; x < 50; ++x) {
            if (const auto placed = terrain.treeAt(x * 311, z * 311)) {
                ++trees;
                ++kinds[static_cast<usize>(placed->tree.kind)];
            }
        }
    }
    usize seen = 0;
    for (const usize count : kinds) {
        seen += count > 0 ? 1U : 0U;
    }
    MESSAGE(trees << " trees of " << seen << " kinds");
    CHECK(seen == kTreeKindCount);
}

TEST_CASE("A tree's crown reaches the height the far terrain expects") {
    const Terrain terrain(blocks(), 12345, PlanetPreset{}, false);
    auto states = std::make_unique<States>();
    usize checked = 0;
    for (i64 cell = 0; cell < 4000 && checked < 40; ++cell) {
        const auto placed = terrain.treeAt(cell, 3);
        if (!placed) {
            continue;
        }
        const i64 top = placed->ground + placed->tree.height;
        const vxe::BlockPos block{.x = placed->tree.crownX, .y = top, .z = placed->tree.crownZ};
        terrain.generate(vxe::toChunk(block), *states);
        const vxe::voxel::BlockStateId state = (*states)[vxe::toLocal(block).index()];
        CHECK(state != vxe::voxel::kAirState);
        ++checked;
    }
    CHECK(checked > 10);
}

TEST_CASE("The peak of a large spruce is all leaves") {
    const Terrain terrain(blocks(), 12345, PlanetPreset{}, false);
    const vxe::voxel::BlockStateId wood = blocks().trees[static_cast<usize>(TreeKind::TallSpruce)].log;
    auto states = std::make_unique<States>();
    usize checked = 0;
    for (i64 z = -200; z < 200 && checked < 20; ++z) {
        for (i64 x = -200; x < 200 && checked < 20; ++x) {
            const auto placed = terrain.treeAt(x * 7, z * 7);
            if (!placed || placed->tree.kind != TreeKind::TallSpruce || placed->tree.height <= 23) {
                continue;
            }
            // The whole thin top: no wood shows on the tip or beside the leader.
            for (i64 y = 0; y < 2; ++y) {
                const vxe::BlockPos block{
                    .x = placed->tree.x, .y = placed->ground + placed->tree.height - y, .z = placed->tree.z};
                terrain.generate(vxe::toChunk(block), *states);
                CHECK((*states)[vxe::toLocal(block).index()] != wood);
            }
            ++checked;
        }
    }
    CHECK(checked > 3);
}

TEST_CASE("locate finds biomes") {
    const Terrain terrain(blocks(), 12345, PlanetPreset{}, false);
    const auto found = terrain.findBiome(Biome::Plains, 0, 0);
    REQUIRE(found.has_value());
    CHECK(terrain.biomeAt(found->x, found->z) == Biome::Plains);
}

TEST_CASE("Rivers and lakes hold water above the sea, inside their banks") {
    const Terrain terrain(blocks(), 12345);
    usize wet = 0;
    usize spills = 0;
    i64 highest = 0;
    for (i64 z = -500; z < 500; z += 2) {
        for (i64 x = -500; x < 500; x += 2) {
            const i64 level = terrain.waterLevel(x, z);
            if (level <= Terrain::kSeaLevel || terrain.surfaceHeight(x, z) >= level) {
                continue;
            }
            ++wet;
            highest = std::max(highest, level);
            // Water ends at higher ground or at more water, never in mid air.
            // A step down to lower water is a rapid, and fine.
            for (const auto& [dx, dz] : {std::pair<i64, i64>{2, 0}, {-2, 0}, {0, 2}, {0, -2}}) {
                const i64 ground = terrain.surfaceHeight(x + dx, z + dz);
                const bool dry = terrain.waterLevel(x + dx, z + dz) <= ground;
                spills += dry && ground + 3 < level ? 1U : 0U;
            }
        }
    }
    MESSAGE(wet << " wet columns (of every fourth), highest water " << highest - Terrain::kSeaLevel
                << " above the sea, " << spills << " open edges");
    CHECK(wet > 300);
    CHECK(highest > Terrain::kSeaLevel + 8);
    CHECK(spills * 5 < wet); // Of 4 sides per column: under 5% of the edges.
}

TEST_CASE("Water brings life to its banks") {
    const Terrain terrain(blocks(), 12345);
    std::vector<RiverNetwork::Stretch> stretches;
    terrain.rivers().gather(-1500.0, -1500.0, 1500.0, 1500.0, stretches);
    REQUIRE(stretches.size() > 20);
    usize banks = 0;
    usize green = 0;
    for (usize i = 0; i < stretches.size(); i += 7) {
        const RiverNetwork::Stretch& stretch = stretches[i];
        // Beside the river, across its direction of flow.
        const f64 alongX = stretch.bx - stretch.ax;
        const f64 alongZ = stretch.bz - stretch.az;
        const f64 length = std::max(std::hypot(alongX, alongZ), 1.0);
        const f64 side = (stretch.widthA * 0.5) + 3.0;
        const auto x = static_cast<i64>(std::floor(stretch.ax - ((alongZ / length) * side)));
        const auto z = static_cast<i64>(std::floor(stretch.az + ((alongX / length) * side)));
        if (terrain.surfaceHeight(x, z) < terrain.waterLevel(x, z)) {
            continue; // In the water after all.
        }
        ++banks;
        green += blocks().isSoil(terrain.topBlock(x, z)) ? 1U : 0U;
    }
    MESSAGE(green << " of " << banks << " bank columns can grow plants");
    REQUIRE(banks > 10);
    CHECK(green * 2 > banks);
}

TEST_CASE("Threads sharing columns get what one thread builds alone") {
    const Terrain alone(blocks(), 4243, PlanetPreset{}, false);
    std::vector<i64> expected;
    for (i64 i = 0; i < 400; ++i) {
        expected.push_back(alone.surfaceHeight(i * 16, (i % 20) * 16));
    }
    const Terrain shared(blocks(), 4243, PlanetPreset{}, false);
    std::atomic<usize> wrong{0};
    std::vector<std::thread> threads;
    for (i64 worker = 0; worker < 6; ++worker) {
        threads.emplace_back([&, worker] {
            for (i64 round = 0; round < 3; ++round) {
                for (i64 i = 0; i < 400; ++i) {
                    const i64 at = (i + (worker * 67)) % 400;
                    const i64 height = shared.surfaceHeight(at * 16, (at % 20) * 16);
                    wrong += height == expected[static_cast<usize>(at)] ? 0U : 1U;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(wrong.load() == 0);
}

TEST_CASE("Underground holds tunnels, caverns and ore veins in their bands") {
    const Terrain terrain(blocks(), 12345, PlanetPreset{}, false);
    const TerrainBlocks& ids = blocks();
    const vxe::BlockPos land = terrain.spawnColumn();
    const vxe::ChunkPos middle = vxe::toChunk(land);
    auto states = std::make_unique<std::array<vxe::voxel::BlockStateId, vxe::kChunkVolume>>();
    std::size_t solid = 0;
    std::size_t air = 0;
    std::array<std::size_t, 6> ores{};
    const std::array<vxe::voxel::BlockStateId, 6> kinds = {ids.coal, ids.copper,  ids.iron,
                                                           ids.gold, ids.diamond, ids.emerald};
    const std::array<vxe::i64, 6> lowest = {0, -16, -64, -160, -1000, 120};
    const std::array<vxe::i64, 6> highest = {320, 128, 256, 32, -24, 480};
    // A slab of chunks from well below the surface down to deep rock.
    for (vxe::i32 cy = -8; cy <= 2; ++cy) {
        for (vxe::i32 cz = middle.z - 3; cz <= middle.z + 3; ++cz) {
            for (vxe::i32 cx = middle.x - 3; cx <= middle.x + 3; ++cx) {
                const vxe::ChunkPos chunk{.x = cx, .y = cy, .z = cz};
                terrain.generate(chunk, *states);
                const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
                for (vxe::u32 i = 0; i < vxe::kChunkVolume; ++i) {
                    const vxe::LocalPos local = vxe::LocalPos::fromIndex(static_cast<vxe::u16>(i));
                    const vxe::i64 y = origin.y + local.y;
                    if (y > terrain.surfaceHeight(origin.x + local.x, origin.z + local.z) - 12) {
                        continue; // Only rock well under the ground.
                    }
                    const vxe::voxel::BlockStateId state = (*states)[i];
                    if (state == vxe::voxel::kAirState) {
                        ++air;
                        continue;
                    }
                    ++solid;
                    for (std::size_t k = 0; k < kinds.size(); ++k) {
                        if (state == kinds.at(k)) {
                            ++ores.at(k);
                            CHECK(y >= lowest.at(k));
                            CHECK(y <= highest.at(k));
                        }
                    }
                }
            }
        }
    }
    const double caves = static_cast<double>(air) / static_cast<double>(air + solid);
    MESSAGE("Cave share " << caves << ", ores: coal " << ores[0] << ", copper " << ores[1] << ", iron "
                          << ores[2] << ", gold " << ores[3] << ", diamond " << ores[4]);
    // Caves take a few percent of the rock, not most of it and not none.
    CHECK(caves > 0.01);
    CHECK(caves < 0.25);
    CHECK(ores[0] > 0);
    CHECK(ores[1] > 0);
    CHECK(ores[2] > 0);
    CHECK(ores[3] > 0);
    CHECK(ores[4] > 0);
}
