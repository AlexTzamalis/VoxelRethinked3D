#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/voxel/mesher.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <utility>
#include <vector>

using namespace vxe;
using voxel::BlockFace;
using voxel::BlockRenderInfo;
using voxel::BlockStateId;
using voxel::ChunkMesh;
using voxel::ChunkSnapshot;
using voxel::MeshLayer;
using voxel::MeshOptions;
using voxel::PackedQuad;
using voxel::RenderLayer;

namespace {

constexpr BlockStateId kAir = 0;
constexpr BlockStateId kStone = 1;
constexpr BlockStateId kGrass = 2;    // Its top has its own texture.
constexpr BlockStateId kWater = 3;    // Translucent, hides faces against water.
constexpr BlockStateId kGlass = 4;    // Translucent, hides faces against glass.
constexpr BlockStateId kLeaves = 5;   // Cutout, shows faces between leaves.
constexpr BlockStateId kPlant = 6;    // Crossed planes, 4 variations from layer 60.
constexpr BlockStateId kBushy = 7;    // Leaves with a bushy plane at layer 70.
constexpr BlockStateId kSnow = 8;     // A slab 4 sixteenths tall, texture 80.
constexpr BlockStateId kDeepSnow = 9; // 8 sixteenths.

std::shared_ptr<voxel::BlockRegistryView> testBlocks() {
    voxel::BlockRegistryBuilder builder;
    const auto add = [&](RenderLayer layer, bool cullsSame, u16 texture, u16 topTexture) {
        BlockRenderInfo info{.layer = layer, .cullsSameType = cullsSame};
        info.faceTextureLayer.fill(texture);
        info.faceTextureLayer[static_cast<usize>(BlockFace::PosY)] = topTexture;
        REQUIRE(builder.add(info));
    };
    add(RenderLayer::Opaque, true, 10, 10);
    add(RenderLayer::Opaque, true, 20, 21);
    add(RenderLayer::Translucent, true, 30, 30);
    add(RenderLayer::Translucent, true, 40, 40);
    add(RenderLayer::Cutout, false, 50, 50);
    BlockRenderInfo plant{.layer = RenderLayer::Plant, .lightOpacity = 0, .variationCount = 4};
    plant.faceTextureLayer.fill(60);
    REQUIRE(builder.add(plant));
    BlockRenderInfo bushy{.layer = RenderLayer::Cutout, .cullsSameType = false, .bushyLayer = 71};
    bushy.faceTextureLayer.fill(55);
    REQUIRE(builder.add(bushy));
    for (const u8 height : {u8{4}, u8{8}}) {
        BlockRenderInfo snow{.layer = RenderLayer::Opaque, .lightOpacity = 0, .height = height};
        snow.faceTextureLayer.fill(80);
        REQUIRE(builder.add(snow));
    }
    return builder.build();
}

// A snapshot of all air (or `fill`), with a setter in chunk local
// coordinates (-1 to 16).
struct TestSnapshot {
    ChunkSnapshot snapshot;

    explicit TestSnapshot(BlockStateId fill = kAir) {
        snapshot.states.fill(fill);
        snapshot.light.fill(voxel::kOpenSkyLight); // What an unlit chunk holds.
    }
    void set(i32 x, i32 y, i32 z, BlockStateId state) {
        snapshot.states[ChunkSnapshot::index(x, y, z)] = state;
    }
    void fillChunk(BlockStateId state) {
        for (i32 y = 0; y < 16; ++y) {
            for (i32 z = 0; z < 16; ++z) {
                for (i32 x = 0; x < 16; ++x) {
                    set(x, y, z, state);
                }
            }
        }
    }
};

ChunkMesh meshOf(const TestSnapshot& snapshot, const MeshOptions& options = {}) {
    static const auto kBlocks = testBlocks();
    voxel::Mesher mesher;
    ChunkMesh mesh;
    mesher.mesh(snapshot.snapshot, *kBlocks, options, mesh);
    return mesh;
}

// The blocks (chunk local) a quad covers, as positions of the face's block.
template <typename Fn>
void forEachCoveredBlock(const PackedQuad& quad, const Fn& fn) {
    const voxel::FaceAxes axes = voxel::faceAxes(quad.face());
    const std::array<u32, 3> origin = {quad.x(), quad.y(), quad.z()};
    for (u32 dv = 0; dv < quad.height(); ++dv) {
        for (u32 du = 0; du < quad.width(); ++du) {
            std::array<i32, 3> block{};
            block.at(axes.normal) = static_cast<i32>(origin.at(axes.normal)) - (axes.positive ? 1 : 0);
            block.at(axes.u) = static_cast<i32>(origin.at(axes.u) + du);
            block.at(axes.v) = static_cast<i32>(origin.at(axes.v) + dv);
            fn(block);
        }
    }
}

const PackedQuad* findQuad(const ChunkMesh& mesh, MeshLayer layer, BlockFace face, std::array<i32, 3> block) {
    for (const PackedQuad& quad : mesh.group(layer, face)) {
        bool found = false;
        forEachCoveredBlock(quad,
                            [&](const std::array<i32, 3>& covered) { found = found || covered == block; });
        if (found) {
            return &quad;
        }
    }
    return nullptr;
}

constexpr std::array<std::array<i32, 3>, 6> kNormals = {
    {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};

} // namespace

TEST_CASE("Connectivity bits number the 15 face pairs") {
    u32 all = 0;
    for (u32 a = 0; a < 6; ++a) {
        for (u32 b = a + 1; b < 6; ++b) {
            const u16 bit = voxel::connectivityBit(static_cast<BlockFace>(a), static_cast<BlockFace>(b));
            CHECK((all & bit) == 0);
            CHECK(bit == voxel::connectivityBit(static_cast<BlockFace>(b), static_cast<BlockFace>(a)));
            all |= bit;
        }
    }
    CHECK(all == voxel::kAllConnected);
}

TEST_CASE("A single block makes six unit quads") {
    TestSnapshot snapshot;
    snapshot.set(3, 4, 5, kGrass);
    snapshot.snapshot.position = {.x = 7, .y = -2, .z = 9};
    snapshot.snapshot.version = 42;
    const ChunkMesh mesh = meshOf(snapshot);
    CHECK(mesh.position == ChunkPos{.x = 7, .y = -2, .z = 9});
    CHECK(mesh.version == 42);
    REQUIRE(mesh.quads.size() == 6);
    CHECK(mesh.layer(MeshLayer::Opaque).size() == 6);
    for (const BlockFace face : {BlockFace::PosX, BlockFace::NegX, BlockFace::PosY, BlockFace::NegY,
                                 BlockFace::PosZ, BlockFace::NegZ}) {
        const auto group = mesh.group(MeshLayer::Opaque, face);
        REQUIRE(group.size() == 1);
        const PackedQuad& quad = group[0];
        CHECK(quad.face() == face);
        CHECK(quad.width() == 1);
        CHECK(quad.height() == 1);
        CHECK(quad.textureLayer() == (face == BlockFace::PosY ? 21U : 20U));
        CHECK(quad.skyLight() == 15);
        CHECK(quad.blockLight() == 0);
        CHECK_FALSE(quad.flipped());
        for (u32 corner = 0; corner < 4; ++corner) {
            CHECK(quad.ao(corner) == 3);
        }
    }
    // +X sits on the block's far side, -X on its near side.
    CHECK(mesh.group(MeshLayer::Opaque, BlockFace::PosX)[0].x() == 4);
    CHECK(mesh.group(MeshLayer::Opaque, BlockFace::NegX)[0].x() == 3);
    CHECK(mesh.group(MeshLayer::Opaque, BlockFace::PosY)[0].y() == 5);
    CHECK(mesh.boundsMin == std::array<u8, 3>{3, 4, 5});
    CHECK(mesh.boundsMax == std::array<u8, 3>{4, 5, 6});
    CHECK(mesh.connectivity == voxel::kAllConnected);
}

TEST_CASE("Greedy merging grows quads across equal faces") {
    TestSnapshot bar;
    bar.set(2, 2, 2, kStone);
    bar.set(3, 2, 2, kStone);
    const ChunkMesh merged = meshOf(bar);
    CHECK(merged.quads.size() == 6); // The shared faces are hidden, the long sides merge.
    CHECK(merged.group(MeshLayer::Opaque, BlockFace::PosY)[0].width() == 2);

    TestSnapshot solid;
    solid.fillChunk(kStone);
    const ChunkMesh full = meshOf(solid);
    REQUIRE(full.quads.size() == 6);
    for (const PackedQuad& quad : full.quads) {
        CHECK(quad.width() == 16);
        CHECK(quad.height() == 16);
    }
    CHECK(full.boundsMin == std::array<u8, 3>{0, 0, 0});
    CHECK(full.boundsMax == std::array<u8, 3>{16, 16, 16});
    CHECK(full.connectivity == 0);
    CHECK(meshOf(solid, {.greedyMerge = false}).quads.size() == 6 * 256);

    // Different states with the same texture still do not merge (Section 10.2).
    TestSnapshot mixed;
    mixed.set(2, 2, 2, kStone);
    mixed.set(3, 2, 2, kGrass);
    CHECK(meshOf(mixed).group(MeshLayer::Opaque, BlockFace::NegY).size() == 2);
}

TEST_CASE("Neighbor chunks hide border faces through the snapshot border") {
    TestSnapshot buried(kStone); // Every neighbor is stone.
    buried.fillChunk(kStone);
    const ChunkMesh mesh = meshOf(buried);
    CHECK(mesh.empty());
    CHECK(mesh.boundsMin[0] > mesh.boundsMax[0]);

    const TestSnapshot air;
    const ChunkMesh nothing = meshOf(air);
    CHECK(nothing.empty());
    CHECK(nothing.connectivity == voxel::kAllConnected);

    TestSnapshot edge;
    edge.set(15, 0, 0, kStone);
    edge.set(16, 0, 0, kStone); // In the +X neighbor.
    CHECK(meshOf(edge).group(MeshLayer::Opaque, BlockFace::PosX).empty());
}

TEST_CASE("Plants are two crossed planes and hide nothing") {
    TestSnapshot field;
    field.set(5, 5, 5, kPlant);
    field.set(5, 4, 5, kStone);
    const ChunkMesh mesh = meshOf(field);
    const auto plants = mesh.group(MeshLayer::Plants, BlockFace::PosX);
    REQUIRE(plants.size() == 2);
    CHECK((plants[0].word0 >> 23U & 7U) == 6);
    CHECK((plants[1].word0 >> 23U & 7U) == 7);
    CHECK(plants[0].x() == 5);
    CHECK(plants[0].y() == 5);
    CHECK(plants[0].textureLayer() >= 60);
    CHECK(plants[0].textureLayer() < 64);
    // The stone's top under the plant still shows.
    CHECK(mesh.layer(MeshLayer::Opaque).size() == 6);
    CHECK(mesh.connectivity == voxel::kAllConnected);
}

TEST_CASE("A plant's variation comes from its position and the world") {
    const auto layerAt = [](i32 x, i32 y, i32 z, u64 seed) {
        TestSnapshot field;
        field.set(x, y, z, kPlant);
        const MeshOptions options{.variationSeed = seed};
        return meshOf(field, options).group(MeshLayer::Plants, BlockFace::PosX)[0].textureLayer();
    };
    // The same spot always looks the same.
    CHECK(layerAt(3, 2, 9, 42) == layerAt(3, 2, 9, 42));
    // Both halves of a tall plant agree: height does not count.
    CHECK(layerAt(3, 2, 9, 42) == layerAt(3, 3, 9, 42));
    // Every variation shows up, and a world has its own pattern.
    std::array<usize, 4> seen{};
    usize different = 0;
    for (i32 z = 0; z < 16; ++z) {
        for (i32 x = 0; x < 16; ++x) {
            ++seen.at(layerAt(x, 0, z, 42) - 60);
            different += layerAt(x, 0, z, 42) != layerAt(x, 0, z, 43) ? 1U : 0U;
        }
    }
    for (const usize count : seen) {
        CHECK(count > 16);
    }
    CHECK(different > 100);
}

TEST_CASE("Advanced leaves add bushy planes to leaves that show") {
    TestSnapshot tree;
    tree.fillChunk(kBushy);
    CHECK(meshOf(tree).group(MeshLayer::Plants, BlockFace::PosX).empty()); // The setting is off.
    const MeshOptions advanced{.advancedLeaves = true};
    const ChunkMesh mesh = meshOf(tree, advanced);
    const auto planes = mesh.group(MeshLayer::Plants, BlockFace::PosX);
    REQUIRE_FALSE(planes.empty());
    CHECK(planes[0].textureLayer() == 70);
    CHECK((planes[0].word0 >> 31U) == 1);

    // Leaves with leaves all around show nothing and add nothing.
    TestSnapshot buried(kBushy);
    CHECK(meshOf(buried, advanced).group(MeshLayer::Plants, BlockFace::PosX).empty());
}

TEST_CASE("Translucent and cutout blocks follow their culling rules") {
    TestSnapshot pool;
    for (i32 y = 0; y < 8; ++y) {
        for (i32 z = 0; z < 16; ++z) {
            for (i32 x = 0; x < 16; ++x) {
                pool.set(x, y, z, kWater);
            }
        }
    }
    const ChunkMesh water = meshOf(pool);
    CHECK(water.layer(MeshLayer::Opaque).empty());
    CHECK(water.layer(MeshLayer::Translucent).size() == 6); // Water hides faces against water.

    TestSnapshot glassOnWater;
    glassOnWater.set(4, 4, 4, kWater);
    glassOnWater.set(5, 4, 4, kGlass);
    const ChunkMesh pair = meshOf(glassOnWater);
    CHECK(pair.group(MeshLayer::Translucent, BlockFace::PosX).size() == 2); // Water's +X and glass' own.
    CHECK(pair.group(MeshLayer::Translucent, BlockFace::NegX).size() == 2);

    TestSnapshot stoneInWater;
    stoneInWater.set(4, 4, 4, kStone);
    stoneInWater.set(5, 4, 4, kWater);
    const ChunkMesh wet = meshOf(stoneInWater);
    CHECK(wet.group(MeshLayer::Opaque, BlockFace::PosX).size() == 1);  // Seen through the water.
    CHECK(wet.group(MeshLayer::Translucent, BlockFace::NegX).empty()); // Water against stone: hidden.

    TestSnapshot leaves;
    leaves.set(4, 4, 4, kLeaves);
    leaves.set(5, 4, 4, kLeaves);
    const ChunkMesh fancy = meshOf(leaves);
    CHECK(fancy.layer(MeshLayer::Cutout).size() == 8); // Faces between leaves stay.
    const ChunkMesh fast = meshOf(leaves, {.fancyLeaves = false});
    CHECK(fast.layer(MeshLayer::Cutout).empty());
    CHECK(fast.layer(MeshLayer::Opaque).size() == 6); // Solid leaves merge like stone.
}

TEST_CASE("Ambient occlusion darkens corners next to blocks and flips the diagonal") {
    TestSnapshot snapshot;
    for (i32 z = 0; z < 16; ++z) {
        for (i32 x = 0; x < 16; ++x) {
            snapshot.set(x, 0, z, kStone); // A floor.
        }
    }
    snapshot.set(5, 1, 5, kStone); // A block on it.
    const ChunkMesh mesh = meshOf(snapshot);

    // The floor cell east of the block: its -U side (corners 0 and 3) is the block.
    const PackedQuad* east = findQuad(mesh, MeshLayer::Opaque, BlockFace::PosY, {6, 0, 5});
    REQUIRE(east != nullptr);
    CHECK(east->ao(0) == 2);
    CHECK(east->ao(1) == 3);
    CHECK(east->ao(2) == 3);
    CHECK(east->ao(3) == 2);
    CHECK_FALSE(east->flipped());

    // Diagonal neighbors: only one corner touches the block.
    const PackedQuad* northEast = findQuad(mesh, MeshLayer::Opaque, BlockFace::PosY, {6, 0, 6});
    REQUIRE(northEast != nullptr);
    CHECK(northEast->ao(0) == 2);
    CHECK_FALSE(northEast->flipped()); // The default diagonal already runs through corner 0.
    const PackedQuad* northWest = findQuad(mesh, MeshLayer::Opaque, BlockFace::PosY, {4, 0, 6});
    REQUIRE(northWest != nullptr);
    CHECK(northWest->ao(1) == 2);
    CHECK(northWest->flipped()); // The diagonal moves to run through corner 1.

    // Far from the block the floor is one bright merged quad region.
    const PackedQuad* far = findQuad(mesh, MeshLayer::Opaque, BlockFace::PosY, {12, 0, 12});
    REQUIRE(far != nullptr);
    CHECK(far->ao(0) == 3);
    CHECK(far->width() * far->height() > 1);
}

namespace {

// An independent, slow mesher for checking the fast one: decides for every
// face of every block whether it is visible and which AO it needs, then
// compares with what the quads of a mesh cover.
struct BruteForce {
    const ChunkSnapshot* snapshot = nullptr;
    const voxel::BlockRegistryView* blocks = nullptr;
    MeshOptions options;

    [[nodiscard]] RenderLayer kindOf(BlockStateId state) const {
        const RenderLayer layer = blocks->info(state).layer;
        return layer == RenderLayer::Cutout && !options.fancyLeaves ? RenderLayer::Opaque : layer;
    }
    [[nodiscard]] u32 opaqueAt(const std::array<i32, 3>& p) const {
        return kindOf(snapshot->at(p[0], p[1], p[2])) == RenderLayer::Opaque ? 1U : 0U;
    }
    [[nodiscard]] static usize faceIndex(usize group, const std::array<i32, 3>& b) {
        return (group * kChunkVolume) + (static_cast<usize>(b[1]) * 256) + (static_cast<usize>(b[2]) * 16) +
               static_cast<usize>(b[0]);
    }

    // AO of one corner of the face of block `b`, per Section 10.3.
    [[nodiscard]] u32 cornerAo(const std::array<i32, 3>& b, BlockFace face, i32 du, i32 dv) const {
        const voxel::FaceAxes axes = voxel::faceAxes(face);
        std::array<i32, 3> sideU = b;
        sideU[axes.normal] += axes.positive ? 1 : -1;
        std::array<i32, 3> sideV = sideU;
        std::array<i32, 3> corner = sideU;
        sideU[axes.u] += du;
        sideV[axes.v] += dv;
        corner[axes.u] += du;
        corner[axes.v] += dv;
        const u32 s1 = opaqueAt(sideU);
        const u32 s2 = opaqueAt(sideV);
        return s1 != 0 && s2 != 0 ? 0U : 3U - (s1 + s2 + opaqueAt(corner));
    }

    // How many quads cover each face (layer and face group, then block),
    // and how many covered faces disagree with their quad's texture or AO.
    [[nodiscard]] std::pair<std::vector<u8>, usize> coverage(const ChunkMesh& mesh) const {
        std::vector<u8> covered(voxel::kFaceGroupCount * kChunkVolume, 0);
        usize badKeys = 0;
        for (usize g = 0; g < voxel::kFaceGroupCount; ++g) {
            const auto face = static_cast<BlockFace>(g % 6);
            for (const PackedQuad& quad : mesh.group(static_cast<MeshLayer>(g / 6), face)) {
                forEachCoveredBlock(quad, [&](const std::array<i32, 3>& b) {
                    ++covered.at(faceIndex(g, b));
                    const BlockStateId state = snapshot->at(b[0], b[1], b[2]);
                    // Lit by the cell in front of the face (unless merged across light).
                    const voxel::FaceAxes axes = voxel::faceAxes(face);
                    std::array<i32, 3> front = b;
                    front[axes.normal] += axes.positive ? 1 : -1;
                    const u8 light = snapshot->lightAt(front[0], front[1], front[2]);
                    const bool lit =
                        options.mergeAcrossLight || (quad.skyLight() == voxel::skyLightOf(light) &&
                                                     quad.blockLight() == voxel::blockLightOf(light));
                    const bool same =
                        lit && quad.textureLayer() == blocks->info(state).textureLayer(face) &&
                        quad.ao(0) == cornerAo(b, face, -1, -1) && quad.ao(1) == cornerAo(b, face, 1, -1) &&
                        quad.ao(2) == cornerAo(b, face, 1, 1) && quad.ao(3) == cornerAo(b, face, -1, 1);
                    badKeys += same ? 0U : 1U;
                });
            }
        }
        return {std::move(covered), badKeys};
    }

    // 1 for every face that must be drawn, in the layout of coverage().
    [[nodiscard]] std::vector<u8> visibleFaces() const {
        constexpr std::array<RenderLayer, 4> kLayers = {RenderLayer::Opaque, RenderLayer::Cutout,
                                                        RenderLayer::Plant, RenderLayer::Translucent};
        std::vector<u8> visible(voxel::kFaceGroupCount * kChunkVolume, 0);
        for (i32 y = 0; y < 16; ++y) {
            for (i32 z = 0; z < 16; ++z) {
                for (i32 x = 0; x < 16; ++x) {
                    const BlockStateId state = snapshot->at(x, y, z);
                    const RenderLayer kind = kindOf(state);
                    if (kind == RenderLayer::Invisible) {
                        continue;
                    }
                    const auto layer = static_cast<usize>(std::ranges::find(kLayers, kind) - kLayers.begin());
                    for (usize f = 0; f < 6; ++f) {
                        const auto& n = kNormals.at(f);
                        const BlockStateId neighbor = snapshot->at(x + n[0], y + n[1], z + n[2]);
                        const bool hidden = kindOf(neighbor) == RenderLayer::Opaque ||
                                            (kind != RenderLayer::Opaque && neighbor == state &&
                                             blocks->info(state).cullsSameType);
                        visible.at(faceIndex((layer * 6) + f, {x, y, z})) = hidden ? 0 : 1;
                    }
                }
            }
        }
        return visible;
    }
};

} // namespace

TEST_CASE("Greedy quads cover exactly the visible faces, against a brute force mesher") {
    const auto blocks = testBlocks();
    voxel::Mesher mesher;
    ChunkMesh mesh;
    std::mt19937 random(2024); // NOLINT(bugprone-random-generator-seed): the same chunks every run.
    const std::array<BlockStateId, 6> palette = {kAir, kStone, kGrass, kWater, kGlass, kLeaves};
    for (int round = 0; round < 40; ++round) {
        // Mostly air or stone, as in real terrain, plus some of everything.
        const u32 airChance = 20 + (static_cast<u32>(round) * 2);
        TestSnapshot snapshot;
        for (BlockStateId& state : snapshot.snapshot.states) {
            const u32 roll = random() % 100;
            state = roll < airChance ? kAir : palette.at(1 + (roll % 5));
            if (round % 4 == 0 && roll % 3 != 0) {
                state = roll < 50 ? kStone : kAir; // Large uniform areas, where merging matters.
            }
        }
        // Light in patches, so merging has to stop where it changes.
        if (round % 2 == 1) {
            for (usize i = 0; i < snapshot.snapshot.light.size(); ++i) {
                snapshot.snapshot.light.at(i) =
                    voxel::packLight(static_cast<u8>(15 - ((i / 97) % 3)), static_cast<u8>((i / 211) % 4));
            }
        }
        const MeshOptions options{
            .greedyMerge = round % 5 != 4, .mergeAcrossLight = round % 7 == 3, .fancyLeaves = round % 3 != 2};
        mesher.mesh(snapshot.snapshot, *blocks, options, mesh);

        const BruteForce reference{
            .snapshot = &snapshot.snapshot, .blocks = blocks.get(), .options = options};
        const auto [covered, badKeys] = reference.coverage(mesh);
        const std::vector<u8> visible = reference.visibleFaces();
        CHECK(badKeys == 0);
        CHECK(covered == visible);
        const auto faces = static_cast<usize>(std::ranges::count(visible, u8{1}));
        if (options.greedyMerge) {
            CHECK(mesh.quads.size() <= faces);
        } else {
            CHECK(mesh.quads.size() == faces);
        }
    }
}

TEST_CASE("Faces take the light of the cell in front, and merge only where it is the same") {
    TestSnapshot snapshot;
    for (i32 z = 0; z < 16; ++z) {
        for (i32 x = 0; x < 16; ++x) {
            snapshot.set(x, 0, z, kStone); // A floor.
            // Above the floor: sky 15 on the left half, sky 4 with block
            // light 9 on the right half.
            snapshot.snapshot.light[ChunkSnapshot::index(x, 1, z)] =
                x < 8 ? voxel::kOpenSkyLight : voxel::packLight(4, 9);
        }
    }
    const ChunkMesh mesh = meshOf(snapshot);
    const auto top = mesh.group(MeshLayer::Opaque, BlockFace::PosY);
    REQUIRE(top.size() == 2); // One quad per light, not one for the whole floor.
    for (const PackedQuad& quad : top) {
        const bool left = quad.x() < 8;
        CHECK(quad.width() == 8);
        CHECK(quad.skyLight() == (left ? 15U : 4U));
        CHECK(quad.blockLight() == (left ? 0U : 9U));
    }
    // Merging across light makes it one quad again, with one of the lights.
    const auto blocks = testBlocks();
    voxel::Mesher mesher;
    ChunkMesh merged;
    mesher.mesh(snapshot.snapshot, *blocks, {.mergeAcrossLight = true}, merged);
    CHECK(merged.group(MeshLayer::Opaque, BlockFace::PosY).size() == 1);
}

TEST_CASE("A checkerboard cannot merge at all") {
    TestSnapshot snapshot;
    for (i32 y = 0; y < 16; ++y) {
        for (i32 z = 0; z < 16; ++z) {
            for (i32 x = 0; x < 16; ++x) {
                if (((x + y + z) & 1) == 0) {
                    snapshot.set(x, y, z, kStone);
                }
            }
        }
    }
    CHECK(meshOf(snapshot).quads.size() == 2048 * 6);
}

TEST_CASE("Connectivity follows open paths between chunk faces") {
    TestSnapshot wall;
    for (i32 y = 0; y < 16; ++y) {
        for (i32 z = 0; z < 16; ++z) {
            wall.set(8, y, z, kStone);
        }
    }
    const u16 split = meshOf(wall).connectivity;
    CHECK((split & voxel::connectivityBit(BlockFace::PosX, BlockFace::NegX)) == 0);
    CHECK(split == (voxel::kAllConnected & ~voxel::connectivityBit(BlockFace::PosX, BlockFace::NegX)));

    TestSnapshot cave;
    cave.fillChunk(kStone);
    for (i32 x = 0; x < 16; ++x) {
        cave.set(x, 8, 8, kAir); // A tunnel straight through along X...
    }
    for (i32 y = 3; y < 6; ++y) {
        cave.set(3, y, 3, kAir); // ...and a closed pocket.
    }
    CHECK(meshOf(cave).connectivity == voxel::connectivityBit(BlockFace::PosX, BlockFace::NegX));

    TestSnapshot bend; // An L shaped tunnel from -X to +Y, through water (not opaque).
    bend.fillChunk(kStone);
    for (i32 x = 0; x <= 5; ++x) {
        bend.set(x, 4, 4, kWater);
    }
    for (i32 y = 4; y < 16; ++y) {
        bend.set(5, y, 4, kAir);
    }
    CHECK(meshOf(bend).connectivity == voxel::connectivityBit(BlockFace::NegX, BlockFace::PosY));
}

TEST_CASE("A mesher and a mesh can be reused for many chunks") {
    const auto blocks = testBlocks();
    voxel::Mesher mesher;
    ChunkMesh mesh;
    TestSnapshot big;
    big.fillChunk(kStone);
    TestSnapshot small;
    small.set(1, 1, 1, kGrass);
    for (int i = 0; i < 3; ++i) {
        mesher.mesh(big.snapshot, *blocks, {.greedyMerge = false}, mesh);
        CHECK(mesh.quads.size() == 1536);
        mesher.mesh(small.snapshot, *blocks, {}, mesh);
        CHECK(mesh.quads.size() == 6);
    }
}

TEST_CASE("Mesher settings and stats") {
    config::SettingsRegistry settings;
    const voxel::MesherSettings mesherSettings(settings);
    CHECK(mesherSettings.options() == MeshOptions{});
    REQUIRE(settings.set("Graphics.Terrain.FancyLeaves", config::SettingValue{false}));
    CHECK_FALSE(mesherSettings.options().fancyLeaves);

    stats::StatRegistry registry;
    const auto blocks = testBlocks();
    voxel::Mesher mesher(&registry);
    ChunkMesh mesh;
    TestSnapshot snapshot;
    snapshot.set(1, 1, 1, kStone);
    mesher.mesh(snapshot.snapshot, *blocks, {}, mesh);
    mesher.mesh(snapshot.snapshot, *blocks, {}, mesh);
    registry.endFrame(0.016);
    CHECK(registry.value(registry.find("Voxel.Mesher.Chunks")) == 2.0);
    CHECK(registry.value(registry.find("Voxel.Mesher.Quads")) == 12.0);
}

TEST_CASE("Translucent quads sort back to front") {
    TestSnapshot snapshot;
    snapshot.set(1, 1, 1, kWater);
    snapshot.set(8, 1, 1, kWater);
    snapshot.set(14, 1, 1, kWater);
    ChunkMesh mesh = meshOf(snapshot);
    std::vector<PackedQuad> quads(mesh.layer(MeshLayer::Translucent).begin(),
                                  mesh.layer(MeshLayer::Translucent).end());
    REQUIRE(quads.size() == 18);
    voxel::sortBackToFront(quads, Vec3(0.0F, 1.5F, 1.5F));
    CHECK(quads.front().x() >= 14); // The farthest water block comes first...
    CHECK(quads.back().x() <= 2);   // ...the nearest last.
}

TEST_CASE("Slabs carry their height, hide nothing and hide sides against taller slabs") {
    TestSnapshot field;
    field.set(5, 4, 5, kStone);
    field.set(5, 5, 5, kSnow);
    field.set(6, 4, 5, kStone);
    field.set(6, 5, 5, kDeepSnow);
    const ChunkMesh mesh = meshOf(field);
    const auto height = [](const PackedQuad& quad) { return (quad.word0 >> 27U) & 15U; };
    // The stones' tops still show under the snow; nothing of the snow is cut.
    REQUIRE(findQuad(mesh, MeshLayer::Opaque, BlockFace::PosY, {5, 4, 5}) != nullptr);
    const PackedQuad* top = findQuad(mesh, MeshLayer::Opaque, BlockFace::PosY, {5, 5, 5});
    REQUIRE(top != nullptr);
    CHECK(height(*top) == 4);
    CHECK(top->textureLayer() == 80);
    // On stone, so no bottom face.
    CHECK(findQuad(mesh, MeshLayer::Opaque, BlockFace::NegY, {5, 5, 5}) == nullptr);
    // The thin slab's side towards the deeper one is hidden, the deeper one's is not.
    CHECK(findQuad(mesh, MeshLayer::Opaque, BlockFace::PosX, {5, 5, 5}) == nullptr);
    const PackedQuad* deepSide = findQuad(mesh, MeshLayer::Opaque, BlockFace::NegX, {6, 5, 5});
    REQUIRE(deepSide != nullptr);
    CHECK(height(*deepSide) == 8);
    REQUIRE(findQuad(mesh, MeshLayer::Opaque, BlockFace::NegX, {5, 5, 5}) != nullptr);
    // Full blocks keep the height bits clear.
    CHECK(height(*findQuad(mesh, MeshLayer::Opaque, BlockFace::PosY, {5, 4, 5})) == 0);
    CHECK(mesh.boundsMax[1] == 6);
}
