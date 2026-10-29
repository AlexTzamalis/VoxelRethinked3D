#include "../test_helpers.hpp"

#include <vxe/core/memory.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/voxel/chunk_codec.hpp>
#include <vxe/voxel/chunk_light.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace vxe;
using voxel::ChunkLight;
using voxel::kDarkLight;
using voxel::kOpenSkyLight;
using voxel::packLight;
using voxel::VoxelWorld;

namespace {

// Light that differs in every block, so each position tells where it came from.
ChunkLight patternLight(u32 seed) {
    std::array<u8, kChunkVolume> values{};
    for (usize i = 0; i < values.size(); ++i) {
        values.at(i) = static_cast<u8>((i * usize{13}) + (usize{seed} * 7));
    }
    ChunkLight light;
    light.assign(values);
    return light;
}

class OneName final : public voxel::BlockStateNames {
public:
    [[nodiscard]] std::string_view nameOf(voxel::BlockStateId /*state*/) const override { return "stone"; }
    [[nodiscard]] std::optional<voxel::BlockStateId> idOf(std::string_view /*name*/) const override {
        return 1;
    }
};

} // namespace

TEST_CASE("Chunk light packs sky and block light and switches modes by itself") {
    CHECK(packLight(15, 3) == 0xF3);
    CHECK(voxel::skyLightOf(0xF3) == 15);
    CHECK(voxel::blockLightOf(0xF3) == 3);

    ChunkLight light;
    CHECK(light.isUniform());
    CHECK(light.get({.x = 4, .y = 5, .z = 6}) == kOpenSkyLight);
    CHECK(light.memoryBytes() == sizeof(ChunkLight));

    const usize before = memory::stats(MemoryTag::Chunks).currentBytes;
    light.set({.x = 1, .y = 2, .z = 3}, kOpenSkyLight); // Same value: stays uniform.
    CHECK(light.isUniform());
    light.set({.x = 1, .y = 2, .z = 3}, packLight(10, 7));
    CHECK_FALSE(light.isUniform());
    CHECK(memory::stats(MemoryTag::Chunks).currentBytes == before + kChunkVolume);
    CHECK(light.sky({.x = 1, .y = 2, .z = 3}) == 10);
    CHECK(light.block({.x = 1, .y = 2, .z = 3}) == 7);
    CHECK(light.get({.x = 0, .y = 0, .z = 0}) == kOpenSkyLight);
    CHECK(light.memoryBytes() == sizeof(ChunkLight) + kChunkVolume);

    CHECK_FALSE(light.compact()); // Still differs.
    light.set({.x = 1, .y = 2, .z = 3}, kOpenSkyLight);
    CHECK(light.compact());
    CHECK(light.isUniform());
    CHECK(memory::stats(MemoryTag::Chunks).currentBytes == before);

    std::array<u8, kChunkVolume> values{};
    values.fill(kDarkLight);
    light.assign(values);
    CHECK(light.isUniform());
    CHECK(light.uniformValue() == kDarkLight);
    values[100] = packLight(0, 14);
    light.assign(values);
    CHECK_FALSE(light.isUniform());
    CHECK(light.getIndex(100) == packLight(0, 14));

    // Copies and moves keep the values; equality ignores the mode.
    const ChunkLight copy = light;
    CHECK(copy == light);
    ChunkLight moved = ChunkLight(light);
    CHECK(moved == light);
    ChunkLight fullDark;
    values[100] = kDarkLight;
    fullDark.assign(values); // Uniform after all.
    std::array<u8, kChunkVolume> almost = values;
    almost[0] = kOpenSkyLight;
    ChunkLight forcedFull;
    forcedFull.assign(almost);
    forcedFull.setIndex(0, kDarkLight); // Full storage holding all dark.
    CHECK_FALSE(forcedFull.isUniform());
    CHECK(forcedFull == fullDark);
    CHECK(forcedFull == ChunkLight(kDarkLight));
    CHECK_FALSE(forcedFull == ChunkLight());
}

TEST_CASE("Light in the world: reads, writes and which neighbors see a change") {
    VoxelWorld world;
    for (i32 x = -1; x <= 1; ++x) {
        world.insertChunk({.x = x, .y = 0, .z = 0}, {}, ChunkLight(kDarkLight));
    }
    CHECK(world.getLight({.x = 3, .y = 3, .z = 3}) == kDarkLight);
    CHECK(world.getLight({.x = 3, .y = 99, .z = 3}, 0x11) == 0x11); // Not loaded.
    const auto version = [&world](i32 x) { return world.findChunk({.x = x, .y = 0, .z = 0})->version(); };

    // Inside the chunk: only it changes.
    u64 west = version(-1);
    u64 middle = version(0);
    u64 east = version(1);
    REQUIRE(world.setLight({.x = 8, .y = 8, .z = 8}, packLight(0, 12)));
    CHECK(world.getLight({.x = 8, .y = 8, .z = 8}) == packLight(0, 12));
    CHECK(version(0) > middle);
    CHECK(version(-1) == west);
    CHECK(version(1) == east);
    CHECK_FALSE(world.findChunk({})->isDirty(voxel::kDirtySave)); // Light is derived, not saved as an edit.

    // On the +X border: the east neighbor's snapshot holds it.
    REQUIRE(world.setLight({.x = 15, .y = 8, .z = 8}, packLight(0, 9)));
    CHECK(version(1) > east);
    CHECK(version(-1) == west);
    CHECK_FALSE(world.setLight({.x = 99, .y = 8, .z = 8}, 0));

    // Replacing a chunk's light marks only neighbors whose border changed.
    ChunkLight next = world.findChunk({})->light();
    next.set({.x = 8, .y = 9, .z = 8}, packLight(0, 4)); // Interior.
    west = version(-1);
    east = version(1);
    middle = version(0);
    REQUIRE(world.replaceLight({}, next));
    CHECK(version(0) > middle);
    CHECK(version(-1) == west);
    CHECK(version(1) == east);
    next.set({.x = 0, .y = 9, .z = 8}, packLight(0, 5)); // -X face.
    REQUIRE(world.replaceLight({}, next));
    CHECK(version(-1) > west);
    CHECK(version(1) == east);
    middle = version(0);
    REQUIRE(world.replaceLight({}, next)); // No change at all.
    CHECK(version(0) == middle);
    CHECK_FALSE(world.replaceLight({.x = 5, .y = 5, .z = 5}, next));
}

TEST_CASE("Snapshots copy light from the chunk and every neighbor") {
    VoxelWorld world;
    std::mt19937 random(9); // NOLINT(bugprone-random-generator-seed): the same layout every run.
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (random() % 5 == 0) {
                    continue; // Missing neighbor.
                }
                const u32 kind = random() % 3;
                ChunkLight light = patternLight(static_cast<u32>(((dy + 1) * 9) + ((dz + 1) * 3) + dx + 1));
                if (kind == 0) {
                    light = ChunkLight(kDarkLight);
                } else if (kind == 1) {
                    light = ChunkLight();
                }
                world.insertChunk({.x = dx, .y = dy, .z = dz}, {}, std::move(light));
            }
        }
    }
    world.insertChunk({}, {}, patternLight(99));
    auto snapshot = std::make_unique<voxel::ChunkSnapshot>();
    REQUIRE(world.createSnapshot({}, *snapshot, voxel::kAirState, 0x77));
    for (i32 y = -1; y <= 16; ++y) {
        for (i32 z = -1; z <= 16; ++z) {
            for (i32 x = -1; x <= 16; ++x) {
                REQUIRE(snapshot->lightAt(x, y, z) == world.getLight({.x = x, .y = y, .z = z}, 0x77));
            }
        }
    }

    // All lit alike: the fast path fills the box.
    VoxelWorld dark;
    dark.insertChunk({}, {}, ChunkLight(kDarkLight));
    REQUIRE(dark.createSnapshot({}, *snapshot, voxel::kAirState, kDarkLight));
    CHECK(std::ranges::all_of(snapshot->light, [](u8 value) { return value == kDarkLight; }));
    REQUIRE(dark.createSnapshot({}, *snapshot)); // Missing neighbors default to open sky.
    CHECK(snapshot->lightAt(16, 0, 0) == kOpenSkyLight);
    CHECK(snapshot->lightAt(15, 0, 0) == kDarkLight);
}

TEST_CASE("Light memory is counted in the world's stats") {
    stats::StatRegistry stats;
    VoxelWorld world({}, &stats);
    world.insertChunk({}, {}, patternLight(1));
    world.insertChunk({.x = 1, .y = 0, .z = 0});
    world.publishStats();
    stats.endFrame(1.0 / 60.0);
    CHECK(stats.value(stats.find("Voxel.World.FullLightChunks")) == doctest::Approx(1.0));
    CHECK(stats.value(stats.find("Voxel.World.LightBytes")) ==
          doctest::Approx(static_cast<f64>((2 * sizeof(ChunkLight)) + kChunkVolume)));
    CHECK(world.storageCounts().lightBytes == (2 * sizeof(ChunkLight)) + kChunkVolume);
    REQUIRE(world.replaceLight({}, ChunkLight(kDarkLight)));
    CHECK(world.storageCounts().fullLight == 0);
    world.removeChunk({});
    CHECK(world.storageCounts().lightBytes == sizeof(ChunkLight));
}

TEST_CASE("Chunk payloads can carry light") {
    const OneName names;
    voxel::ChunkStorage blocks;
    blocks.fill(1);
    for (const ChunkLight& light : {ChunkLight(kDarkLight), patternLight(5)}) {
        io::BinaryWriter writer;
        voxel::encodeChunk(blocks, names, writer, &light);
        auto decoded = voxel::decodeChunk(writer.bytes(), names);
        REQUIRE(decoded.hasValue());
        REQUIRE(decoded.value().light.has_value());
        CHECK(*decoded.value().light == light);
        CHECK(decoded.value().light->isUniform() == light.isUniform());
        // Cut short anywhere in the light section: an error, never a crash.
        const std::span<const std::byte> bytes = writer.bytes();
        for (usize size = bytes.size() - 1; size + 64 > bytes.size() && size > 8; --size) {
            CHECK_FALSE(voxel::decodeChunk(bytes.first(size), names).hasValue());
        }
    }
    io::BinaryWriter without;
    voxel::encodeChunk(blocks, names, without);
    auto decoded = voxel::decodeChunk(without.bytes(), names);
    REQUIRE(decoded.hasValue());
    CHECK_FALSE(decoded.value().light.has_value());
}
