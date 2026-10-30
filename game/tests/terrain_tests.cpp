#include "worldgen/terrain.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

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
    const Terrain terrain(blocks(), 11);
    const Terrain same(blocks(), 11);
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
    const Terrain a(blocks(), 1);
    const Terrain b(blocks(), 2);
    usize different = 0;
    for (i64 i = 0; i < 100; ++i) {
        different += a.surfaceHeight(i * 900, 0) != b.surfaceHeight(i * 900, 0) ? 1U : 0U;
    }
    CHECK(different > 80);
}

TEST_CASE("Columns meet: chunk borders show no step the slope does not explain") {
    for (const u64 seed : {3ULL, 12345ULL}) {
        const Terrain terrain(blocks(), seed);
        i64 worstBorder = 0;
        i64 worstInside = 0;
        for (i64 z = 0; z < 2400; z += 3) {
            i64 last = terrain.surfaceHeight(0, z);
            for (i64 x = 1; x < 2400; ++x) {
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
    const Terrain terrain(blocks(), 2);
    usize changed = 0;
    usize steep = 0;
    f64 lowered = 0.0;
    for (i64 z = 0; z < 300; ++z) {
        for (i64 x = 0; x < 300; ++x) {
            const auto wx = static_cast<f64>(x * 70);
            const auto wz = static_cast<f64>(z * 70);
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
    const Terrain terrain(blocks(), 5);
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
    const Terrain terrain(blocks(), 6);
    auto states = std::make_unique<States>();
    terrain.generate({.x = 0, .y = 180, .z = 0}, *states);
    CHECK(std::ranges::all_of(*states,
                              [](vxe::voxel::BlockStateId state) { return state == vxe::voxel::kAirState; }));
    terrain.generate({.x = 0, .y = Terrain::kFloorY >> 4, .z = 0}, *states);
    CHECK((*states)[0] == blocks().floor);
}

TEST_CASE("Players start on dry land on every seed") {
    for (const u64 seed : {1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL, 12345ULL, 987654321ULL}) {
        const Terrain terrain(blocks(), seed);
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
    // A walk of many chunks through one world finds many kinds of trees,
    // plants and ground, not one flat green.
    const Terrain terrain(ids, 12345);
    usize trees = 0;
    std::array<usize, kTreeKindCount> kinds{};
    for (i64 z = -400; z < 400; ++z) {
        for (i64 x = -400; x < 400; ++x) {
            if (const auto placed = terrain.treeAt(x, z)) {
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
    const Terrain terrain(blocks(), 12345);
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

TEST_CASE("locate finds biomes") {
    const Terrain terrain(blocks(), 12345);
    const auto found = terrain.findBiome(Biome::Plains, 0, 0);
    REQUIRE(found.has_value());
    CHECK(terrain.biomeAt(found->x, found->z) == Biome::Plains);
}
