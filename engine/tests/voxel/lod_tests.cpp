#include "../test_helpers.hpp"

#include <vxe/core/stats.hpp>
#include <vxe/io/file_io.hpp>
#include <vxe/voxel/lod_cache.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace vxe;
using voxel::LodClipmap;
using voxel::LodClipmapDesc;
using voxel::LodSpan;
using voxel::LodTile;
using voxel::LodTileKey;

namespace {

std::filesystem::path freshFolder(std::string_view name) {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "lod" / std::string(name);
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder);
    return folder;
}

// A tile shaped like terrain: rolling hills, with an overhang or a floating
// island in some cells and no ground (void) in a few.
LodTile terrainTile(LodTileKey key, u32 seed) {
    std::mt19937 random(seed);
    voxel::LodTileBuilder builder(key);
    std::vector<LodSpan> spans;
    for (i32 cz = 0; cz < voxel::kLodTileCells; ++cz) {
        for (i32 cx = 0; cx < voxel::kLodTileCells; ++cx) {
            spans.clear();
            const auto ground =
                static_cast<i32>(64.0 + (20.0 * std::sin(cx * 0.1)) + (15.0 * std::cos(cz * 0.07)));
            const u32 roll = random() % 100;
            if (roll < 3) {
                builder.addCell({}); // Void.
                continue;
            }
            if (roll < 10) {
                spans.push_back({.bottomY = ground + 30,
                                 .topY = ground + 34,
                                 .topMaterial = 7,
                                 .sideMaterial = 8,
                                 .skyLight = 15});
            }
            spans.push_back({.bottomY = -1024,
                             .topY = ground,
                             .topMaterial = static_cast<u16>(roll < 50 ? 1 : 2),
                             .sideMaterial = 3,
                             .skyLight = static_cast<u8>(roll < 10 ? 9 : 15)});
            builder.addCell(spans);
        }
    }
    return std::move(builder).build();
}

// Any valid spans, also at the very ends of the int32 range.
std::vector<LodSpan> randomSpans(std::mt19937& random) {
    std::vector<LodSpan> spans;
    const u32 count = random() % (voxel::kLodMaxSpansPerCell + 1);
    i64 top = std::numeric_limits<i32>::max() - static_cast<i64>(random() % 3);
    for (u32 i = 0; i < count; ++i) {
        const i64 height = static_cast<i64>(random() % (i == 0 ? 400000000U : 1000U));
        const i64 bottom = std::max<i64>(top - height, std::numeric_limits<i32>::min());
        spans.push_back({.bottomY = static_cast<i32>(bottom),
                         .topY = static_cast<i32>(top),
                         .topMaterial = static_cast<u16>(random()),
                         .sideMaterial = static_cast<u16>(random()),
                         .skyLight = static_cast<u8>(random() % 16)});
        top = bottom - 2 - static_cast<i64>(random() % 50);
        if (top < i64{std::numeric_limits<i32>::min()} + 2000) {
            break;
        }
    }
    return spans;
}

bool covers(const LodTileKey& key, i64 x, i64 z) {
    return x >= key.originX() && x < key.originX() + key.tileSize() && z >= key.originZ() &&
           z < key.originZ() + key.tileSize();
}

} // namespace

TEST_CASE("LOD tile keys: sizes, origins and the quadtree, also below zero") {
    constexpr LodTileKey kKey{.level = 3, .x = -2, .z = 5};
    static_assert(kKey.cellSize() == 8);
    static_assert(kKey.tileSize() == 512);
    static_assert(kKey.originX() == -1024 && kKey.originZ() == 2560);
    static_assert(voxel::lodTileAt(1, -1, 0) == LodTileKey{.level = 1, .x = -1, .z = 0});
    static_assert(voxel::lodTileAt(1, 127, 128) == LodTileKey{.level = 1, .x = 0, .z = 1});
    static_assert(LodTileKey{.level = 1, .x = -1, .z = -3}.parent() ==
                  LodTileKey{.level = 2, .x = -1, .z = -2});
    for (u32 i = 0; i < 4; ++i) {
        const LodTileKey child = kKey.child(i);
        CHECK(child.level == 2);
        CHECK(child.parent() == kKey);
        // The children tile their parent exactly.
        CHECK(child.originX() == kKey.originX() + ((i & 1U) != 0 ? child.tileSize() : 0));
        CHECK(child.originZ() == kKey.originZ() + ((i & 2U) != 0 ? child.tileSize() : 0));
    }
    std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed): the same data every run.
    for (int i = 0; i < 1000; ++i) {
        const auto level = static_cast<u8>(1 + (random() % 16));
        const i64 x = static_cast<i64>(random()) - (i64{1} << 31U);
        const i64 z = static_cast<i64>(random() % 100000) - 50000;
        const LodTileKey at = voxel::lodTileAt(level, x, z);
        CHECK(covers(at, x, z));
        CHECK(covers(at.parent(), x, z));
    }
}

TEST_CASE("LOD spans must be ordered from the top, apart and not too many") {
    const LodSpan high{.bottomY = 50, .topY = 60};
    const LodSpan low{.bottomY = 0, .topY = 40};
    CHECK(voxel::validLodSpans({}));
    CHECK(voxel::validLodSpans(std::vector{high, low}));
    CHECK_FALSE(voxel::validLodSpans(std::vector{low, high}));                        // Upside down.
    CHECK_FALSE(voxel::validLodSpans(std::vector{LodSpan{.bottomY = 5, .topY = 4}})); // Negative height.
    CHECK_FALSE(voxel::validLodSpans(std::vector{high, LodSpan{.bottomY = 0, .topY = 49}})); // Touching.
    CHECK(voxel::validLodSpans(std::vector{high, LodSpan{.bottomY = 0, .topY = 48}}));
    std::vector<LodSpan> many;
    many.reserve(9);
    for (i32 i = 0; i < 9; ++i) {
        many.push_back({.bottomY = -10 * i, .topY = (-10 * i) + 5});
    }
    CHECK_FALSE(voxel::validLodSpans(many));
    many.pop_back();
    CHECK(voxel::validLodSpans(many));
}

TEST_CASE("Replacing cells gives the same tile as building it that way") {
    std::mt19937 random(11); // NOLINT(bugprone-random-generator-seed): the same data every run.
    const LodTileKey key{.level = 2, .x = 3, .z = -4};
    std::vector<std::vector<LodSpan>> cells(voxel::kLodCellCount);
    for (auto& cell : cells) {
        cell = randomSpans(random);
    }
    voxel::LodTileBuilder builder(key);
    for (const auto& cell : cells) {
        builder.addCell(cell);
    }
    LodTile tile = std::move(builder).build();
    CHECK(tile.key() == key);
    for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
        REQUIRE(std::ranges::equal(tile.cellAt(i), cells[i]));
    }
    CHECK(tile.cell(5, 7).size() == cells[(7 * 64) + 5].size());
    for (int edit = 0; edit < 300; ++edit) {
        const u32 index = random() % voxel::kLodCellCount;
        cells[index] = randomSpans(random);
        tile.setCell(index, cells[index]);
    }
    voxel::LodTileBuilder again(key);
    for (const auto& cell : cells) {
        again.addCell(cell);
    }
    CHECK(tile == std::move(again).build());
    CHECK(tile.memoryBytes() >= tile.spanCount() * sizeof(LodSpan));
}

TEST_CASE("LOD tiles survive the cache file format, extremes included") {
    const u64 world = voxel::lodWorldHash(12345, 1, 99);
    CHECK(world != voxel::lodWorldHash(12345, 2, 99));
    CHECK(world != voxel::lodWorldHash(12346, 1, 99));
    CHECK(world != voxel::lodWorldHash(12345, 1, 98));

    const LodTileKey key{.level = 4, .x = -7, .z = 2};
    const LodTile terrain = terrainTile(key, 1);
    std::vector<std::byte> bytes;
    voxel::encodeLodTile(terrain, world, bytes);
    auto decoded = voxel::decodeLodTile(bytes, world, key);
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value() == terrain);
    MESSAGE("Terrain tile: " << terrain.spanCount() << " spans, " << terrain.memoryBytes()
                             << " bytes in memory, " << bytes.size() << " bytes on disk");
    CHECK(bytes.size() < terrain.spanCount() * 4); // Deltas plus LZ4: a few bytes per span.

    std::mt19937 random(21); // NOLINT(bugprone-random-generator-seed): the same data every run.
    voxel::LodTileBuilder builder(key);
    for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
        builder.addCell(randomSpans(random));
    }
    const LodTile extreme = std::move(builder).build();
    bytes.clear();
    voxel::encodeLodTile(extreme, world, bytes);
    decoded = voxel::decodeLodTile(bytes, world, key);
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value() == extreme);

    // Another world or generator: stale. Another tile: the caller's mistake.
    CHECK(voxel::decodeLodTile(bytes, world + 1, key).error().code() == ErrorCode::VersionMismatch);
    CHECK(voxel::decodeLodTile(bytes, world, key.parent()).error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("Damaged LOD tile files are reported, never read out of bounds") {
    const u64 world = voxel::lodWorldHash(7, 1, 0);
    const LodTileKey key{.level = 1, .x = 0, .z = 0};
    std::vector<std::byte> bytes;
    voxel::encodeLodTile(terrainTile(key, 3), world, bytes);
    for (usize size = 0; size < bytes.size(); size += 1 + (size / 8)) {
        CHECK_FALSE(voxel::decodeLodTile(std::span(bytes).first(size), world, key).hasValue());
    }
    std::mt19937 random(9); // NOLINT(bugprone-random-generator-seed): the same data every run.
    for (int i = 0; i < 300; ++i) {
        std::vector<std::byte> damaged = bytes;
        damaged.at(random() % damaged.size()) ^= static_cast<std::byte>(1U + (random() % 255));
        const auto result = voxel::decodeLodTile(damaged, world, key);
        // A flipped byte in the payload fails the checksum; in the header it
        // fails a check of its own. Either way, no tile comes back.
        CHECK_FALSE(result.hasValue());
    }
}

TEST_CASE("The LOD tile cache keeps tiles per world and treats bad files as misses") {
    const auto folder = freshFolder("cache");
    stats::StatRegistry stats;
    const u64 world = voxel::lodWorldHash(42, 3, 0);
    voxel::LodTileCache cache({.folder = folder, .worldHash = world, .stats = &stats});
    const LodTileKey key{.level = 2, .x = -3, .z = 8};
    CHECK(voxel::LodTileCache::fileName(key) == std::filesystem::path("l2") / "t.-3.8.vxl");

    CHECK_FALSE(cache.load(key).has_value());
    const LodTile tile = terrainTile(key, 4);
    REQUIRE(cache.save(tile).hasValue());
    CHECK(std::filesystem::exists(folder / "l2" / "t.-3.8.vxl"));
    const auto loaded = cache.load(key);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == tile);

    // The same folder seen by a newer generator: every tile is stale.
    voxel::LodTileCache newer(
        {.folder = folder, .worldHash = voxel::lodWorldHash(42, 4, 0), .stats = &stats});
    CHECK_FALSE(newer.load(key).has_value());

    // A damaged file is a miss, and saving replaces it.
    std::vector<std::byte> junk(100, std::byte{0x5A});
    REQUIRE(io::writeFile(folder / voxel::LodTileCache::fileName(key), junk).hasValue());
    CHECK_FALSE(cache.load(key).has_value());
    REQUIRE(cache.save(tile).hasValue());
    CHECK(cache.load(key).has_value());

    stats.endFrame(1.0 / 60.0);
    CHECK(stats.value(stats.find("Voxel.Lod.Cache.Hits")) == doctest::Approx(2.0));
    CHECK(stats.value(stats.find("Voxel.Lod.Cache.Misses")) == doctest::Approx(3.0));
    CHECK(stats.value(stats.find("Voxel.Lod.Cache.Stale")) == doctest::Approx(1.0));
    CHECK(stats.value(stats.find("Voxel.Lod.Cache.Damaged")) == doctest::Approx(1.0));
    CHECK(stats.value(stats.find("Voxel.Lod.Cache.BytesWritten")) > 0.0);
}

TEST_CASE("The clipmap covers every column within reach exactly once, levels stepping by one") {
    for (const i32 half : {2, 4, 6}) {
        const LodClipmapDesc desc{.firstLevel = 1, .levels = 6, .halfExtentTiles = half};
        std::mt19937 random(static_cast<u32>(half));
        for (int camera = 0; camera < 12; ++camera) {
            const i64 cameraX = static_cast<i64>(random() % 4000000) - 2000000;
            const i64 cameraZ = static_cast<i64>(random() % 4000000) - 2000000;
            std::vector<LodTileKey> tiles;
            LodClipmap::tilesAround(desc, cameraX, cameraZ, tiles);
            const usize ring = usize{4} * static_cast<usize>(half) * static_cast<usize>(half);
            CHECK(tiles.size() == ring + (5 * (ring - (ring / 4))));
            CHECK(std::set<LodTileKey>(tiles.begin(), tiles.end()).size() == tiles.size());

            // Guaranteed reach: the coarsest square is off center by at most one tile.
            const i64 reach = (half - 1) * LodTileKey{.level = 6}.tileSize();
            const auto tileAt = [&](i64 x, i64 z) {
                const LodTileKey* found = nullptr;
                usize count = 0;
                for (const LodTileKey& key : tiles) {
                    if (covers(key, x, z)) {
                        found = &key;
                        ++count;
                    }
                }
                CHECK(count <= 1);
                return found;
            };
            for (int sample = 0; sample < 300; ++sample) {
                const i64 x = cameraX - reach + static_cast<i64>(random() % static_cast<u32>(2 * reach));
                const i64 z = cameraZ - reach + static_cast<i64>(random() % static_cast<u32>(2 * reach));
                CHECK(tileAt(x, z) != nullptr);
            }
            if (half < 4) {
                continue; // Small squares may let levels touch two apart.
            }
            // Across every tile edge, the neighbor is at most one level apart.
            for (const LodTileKey& key : tiles) {
                const i64 middle = key.tileSize() / 2;
                const std::array<std::array<i64, 2>, 4> outside = {{
                    {key.originX() - 1, key.originZ() + middle},
                    {key.originX() + key.tileSize(), key.originZ() + middle},
                    {key.originX() + middle, key.originZ() - 1},
                    {key.originX() + middle, key.originZ() + key.tileSize()},
                }};
                for (const auto& point : outside) {
                    if (const LodTileKey* neighbor = tileAt(point[0], point[1]); neighbor != nullptr) {
                        CHECK(std::abs(i32{neighbor->level} - i32{key.level}) <= 1);
                    }
                }
            }
        }
    }
}

TEST_CASE("Clipmap updates report only what changed") {
    LodClipmap clipmap({.firstLevel = 1, .levels = 6, .halfExtentTiles = 4});
    CHECK(clipmap.update(100, -50));
    CHECK(clipmap.added().size() == clipmap.tiles().size());
    CHECK(clipmap.removed().empty());
    CHECK(clipmap.tiles().size() == 64 + (5 * 48));
    CHECK_FALSE(clipmap.update(101, -49)); // Same centers.
    CHECK(clipmap.added().empty());

    std::set<LodTileKey> current(clipmap.tiles().begin(), clipmap.tiles().end());
    i64 x = 100;
    usize changes = 0;
    for (int step = 0; step < 400; ++step) {
        x += 37; // About 15,000 blocks in all.
        if (!clipmap.update(x, -50)) {
            continue;
        }
        ++changes;
        for (const LodTileKey& key : clipmap.removed()) {
            CHECK(current.erase(key) == 1);
        }
        for (const LodTileKey& key : clipmap.added()) {
            CHECK(current.insert(key).second);
        }
        CHECK(current == std::set<LodTileKey>(clipmap.tiles().begin(), clipmap.tiles().end()));
        CHECK(clipmap.contains(clipmap.tiles().front()));
        // A level-1 step changes a ring of level 1 and 2 tiles, not everything.
        CHECK(clipmap.added().size() < clipmap.tiles().size() / 2);
    }
    CHECK(changes > 50);
    CHECK(changes < 200);

    clipmap.setDesc({.firstLevel = 1, .levels = 7, .halfExtentTiles = 4});
    CHECK(clipmap.update(x, -50));
    CHECK(clipmap.added().size() == 48);
    CHECK(clipmap.removed().empty());
}
