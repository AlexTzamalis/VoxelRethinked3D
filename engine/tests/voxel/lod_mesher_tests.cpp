#include "../test_helpers.hpp"

#include <vxe/voxel/lod_mesher.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <random>
#include <tuple>
#include <vector>

using namespace vxe;
using voxel::LodMesh;
using voxel::LodQuad;
using voxel::LodSpan;
using voxel::LodTile;
using voxel::LodTileKey;

namespace {

constexpr i32 kCells = voxel::kLodTileCells;

LodTile flatTile(LodTileKey key, i32 top) {
    voxel::LodTileBuilder builder(key);
    const LodSpan ground{.bottomY = -64, .topY = top, .topMaterial = 3, .sideMaterial = 4};
    for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
        builder.addCell(std::span(&ground, 1));
    }
    return std::move(builder).build();
}

// Up to 3 spans per cell at small heights, so neighbors overlap in every way.
LodTile randomTile(LodTileKey key, u32 seed) {
    std::mt19937 random(seed);
    voxel::LodTileBuilder builder(key);
    std::vector<LodSpan> spans;
    for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
        spans.clear();
        const u32 count = random() % 4;
        i32 top = 40 + static_cast<i32>(random() % 30);
        for (u32 s = 0; s < count; ++s) {
            const i32 bottom = top - static_cast<i32>(random() % 12);
            spans.push_back({.bottomY = bottom,
                             .topY = top,
                             .topMaterial = static_cast<u16>(random() % 3),
                             .sideMaterial = static_cast<u16>(5 + (random() % 2)),
                             .skyLight = static_cast<u8>(12 + (random() % 4))});
            top = bottom - 2 - static_cast<i32>(random() % 8);
        }
        builder.addCell(spans);
    }
    return std::move(builder).build();
}

// Blocks of side face exposed per (face, cell), counted block by block.
std::map<std::tuple<u32, i32, i32>, i64> bruteSides(const LodTile& tile, i64 skirt) {
    const std::array<std::tuple<u32, i32, i32>, 4> directions = {
        {{0U, 1, 0}, {1U, -1, 0}, {4U, 0, 1}, {5U, 0, -1}}};
    std::map<std::tuple<u32, i32, i32>, i64> exposed;
    for (i32 cz = 0; cz < kCells; ++cz) {
        for (i32 cx = 0; cx < kCells; ++cx) {
            for (const auto& [face, dx, dz] : directions) {
                const i32 nx = cx + dx;
                const i32 nz = cz + dz;
                const bool inside = nx >= 0 && nx < kCells && nz >= 0 && nz < kCells;
                i64 count = 0;
                for (const LodSpan& span : tile.cell(static_cast<u32>(cx), static_cast<u32>(cz))) {
                    for (i64 y = span.bottomY; y <= span.topY; ++y) {
                        if (!inside) {
                            count += y > span.topY - skirt ? 1 : 0;
                            continue;
                        }
                        const auto neighbor = tile.cell(static_cast<u32>(nx), static_cast<u32>(nz));
                        const bool covered = std::ranges::any_of(neighbor, [y](const LodSpan& other) {
                            return y >= other.bottomY && y <= other.topY;
                        });
                        count += covered ? 0 : 1;
                    }
                }
                if (count > 0) {
                    exposed[{face, cx, cz}] = count;
                }
            }
        }
    }
    return exposed;
}

// What a mesh covers: top and bottom faces per (face, cell), exposed side
// blocks per (face, cell), and how many covered top or bottom cells match none
// of the cell's spans.
struct Tally { // NOLINT(bugprone-exception-escape): test data; a throwing map ends the test anyway.
    std::map<std::tuple<u32, i32, i32>, i64> tops;
    std::map<std::tuple<u32, i32, i32>, i64> sides;
    usize badTops = 0;
};

Tally tally(const LodTile& tile, const LodMesh& mesh) {
    Tally result;
    for (const LodQuad& quad : mesh.quads) {
        const i32 y = mesh.baseY + static_cast<i32>(quad.y());
        CHECK(y >= mesh.minY);
        CHECK(y + static_cast<i32>(quad.height()) <= mesh.maxY);
        if (quad.face() != 2 && quad.face() != 3) {
            const bool alongX = quad.face() == 4 || quad.face() == 5;
            for (u32 u = 0; u < quad.sizeU(); ++u) {
                const auto cx = static_cast<i32>(quad.x() + (alongX ? u : 0));
                const auto cz = static_cast<i32>(quad.z() + (alongX ? 0 : u));
                result.sides[{quad.face(), cx, cz}] += quad.height();
            }
            continue;
        }
        const auto matches = [&](const LodSpan& span) {
            if (quad.face() == 2) {
                return span.topY + 1 == y && span.topMaterial == quad.material();
            }
            return span.bottomY == y && span.sideMaterial == quad.material();
        };
        for (u32 dz = 0; dz < quad.sizeV(); ++dz) {
            for (u32 dx = 0; dx < quad.sizeU(); ++dx) {
                const auto cx = static_cast<i32>(quad.x() + dx);
                const auto cz = static_cast<i32>(quad.z() + dz);
                ++result.tops[{quad.face(), cx, cz}];
                const auto spans = tile.cell(static_cast<u32>(cx), static_cast<u32>(cz));
                result.badTops += std::ranges::any_of(spans, matches) ? 0U : 1U;
            }
        }
    }
    return result;
}

} // namespace

TEST_CASE("A flat LOD tile is one top and four skirts") {
    voxel::LodMesher mesher;
    LodMesh mesh;
    const LodTileKey key{.level = 2, .x = -1, .z = 3};
    mesher.mesh(flatTile(key, 70), {}, mesh);
    CHECK(mesh.key == key);
    REQUIRE(mesh.quads.size() == 5);
    const LodQuad& top = mesh.quads.front();
    CHECK(top.face() == 2);
    CHECK(top.sizeU() == 64);
    CHECK(top.sizeV() == 64);
    CHECK(top.material() == 3);
    CHECK(mesh.baseY + static_cast<i32>(top.y()) == 71);
    // Skirts: 4 cells of 4 blocks = 16 blocks down from the top, merged
    // along each whole edge.
    for (usize i = 1; i < 5; ++i) {
        const LodQuad& side = mesh.quads[i];
        CHECK(side.face() != 2);
        CHECK(side.sizeU() == 64);
        CHECK(side.height() == 16);
        CHECK(side.material() == 4);
        CHECK(mesh.baseY + static_cast<i32>(side.y()) == 55);
    }
    CHECK(mesh.minY == 55);
    CHECK(mesh.maxY == 71);
}

TEST_CASE("LOD quads cover every top, underside and exposed side exactly once") {
    voxel::LodMesher mesher;
    LodMesh mesh;
    for (u32 round = 0; round < 6; ++round) {
        const LodTileKey key{.level = static_cast<u8>(1 + round), .x = static_cast<i32>(round) - 3, .z = 2};
        const LodTile tile = randomTile(key, 100 + round);
        const voxel::LodMeshOptions options{.skirtCells = 3};
        mesher.mesh(tile, options, mesh);
        const i64 skirt = std::max<i64>(16, 3 * key.cellSize());

        const auto [tops, sides, badTops] = tally(tile, mesh);
        CHECK(badTops == 0);
        // Every span has one top; every span with another below one underside.
        for (i32 cz = 0; cz < kCells; ++cz) {
            for (i32 cx = 0; cx < kCells; ++cx) {
                const usize count = tile.cell(static_cast<u32>(cx), static_cast<u32>(cz)).size();
                const auto topCount = tops.contains({2U, cx, cz}) ? tops.at({2U, cx, cz}) : 0;
                const auto bottomCount = tops.contains({3U, cx, cz}) ? tops.at({3U, cx, cz}) : 0;
                CHECK(static_cast<usize>(topCount) == count);
                CHECK(static_cast<usize>(bottomCount) == (count > 0 ? count - 1 : 0));
            }
        }
        CHECK(sides == bruteSides(tile, skirt));
        MESSAGE("Level " << int{key.level} << ": " << tile.spanCount() << " spans, " << mesh.quads.size()
                         << " quads");
    }
}

TEST_CASE("A LOD mesher can be reused, and empty tiles give no quads") {
    voxel::LodMesher mesher;
    LodMesh mesh;
    voxel::LodTileBuilder builder({.level = 1});
    for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
        builder.addCell({});
    }
    const LodTile empty = std::move(builder).build();
    mesher.mesh(flatTile({.level = 1}, 10), {}, mesh);
    CHECK(mesh.quads.size() == 5);
    mesher.mesh(empty, {}, mesh);
    CHECK(mesh.quads.empty());
    mesher.mesh(flatTile({.level = 1}, 10), {}, mesh);
    CHECK(mesh.quads.size() == 5);
}
