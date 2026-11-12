#include "common/block_catalog.hpp"
#include "common/catalog_state_names.hpp"
#include "server/world_saver.hpp"
#include "server/world_store.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/io/zip.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/voxel/chunk_source.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace vr;

namespace {

// An empty folder of its own for one test.
fs::path freshSaves(const std::string& name) {
    const fs::path folder = fs::temp_directory_path() / "vr_world_tests" / name;
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

WorldInfo world(const std::string& folder, const std::string& name, vxe::i64 lastPlayed) {
    WorldInfo info;
    info.folder = folder;
    info.name = name;
    info.lastPlayedSeconds = lastPlayed;
    return info;
}

} // namespace

TEST_CASE("Seeds: numbers stay as typed, text is hashed") {
    CHECK(seedFromText("12345") == 12345);
    CHECK(seedFromText("18446744073709551615") == 18446744073709551615ULL);
    CHECK(seedFromText("-1") == 18446744073709551615ULL);
    CHECK(seedFromText("hello") == seedFromText("hello"));
    CHECK(seedFromText("hello") != seedFromText("Hello"));
    CHECK(seedFromText("12 monkeys") != 12);
    CHECK(seedFromText(randomSeedText()) != 0);
    CHECK(randomSeedText() != randomSeedText());
}

TEST_CASE("World heights must be multiples of 16 with room between") {
    CHECK(validateWorldHeight(-1024, 3072).hasValue());
    CHECK_FALSE(validateWorldHeight(-1000, 3072).hasValue());
    CHECK_FALSE(validateWorldHeight(0, 0).hasValue());
    CHECK_FALSE(validateWorldHeight(64, 0).hasValue());
}

TEST_CASE("world.toml keeps everything, even seeds beyond TOML integers") {
    const fs::path saves = freshSaves("round_trip");
    WorldInfo info = world("my_world", "My \"Quoted\" World", 1700000000);
    info.seedText = "18446744073709551615";
    info.seed = seedFromText(info.seedText);
    info.worldType = "Islands";
    info.preset = "Default";
    info.minY = -8192;
    info.maxY = 16384;
    info.structures = false;
    info.premapBlocks = 8192;
    info.lodQuality = "Ultra";
    info.createdSeconds = 1600000000;
    info.gameVersion = "0.1.0";
    REQUIRE(saveWorldInfo(saves, info).hasValue());
    const auto loaded = loadWorldInfo(saves, "my_world");
    REQUIRE(loaded.hasValue());
    const WorldInfo& read = loaded.value();
    CHECK(read.name == info.name);
    CHECK(read.seed == info.seed);
    CHECK(read.seedText == info.seedText);
    CHECK(read.worldType == "Islands");
    CHECK(read.minY == -8192);
    CHECK(read.maxY == 16384);
    CHECK_FALSE(read.structures);
    CHECK(read.premapBlocks == 8192);
    CHECK(read.lodQuality == "Ultra");
    CHECK(read.createdSeconds == 1600000000);
    CHECK(read.lastPlayedSeconds == 1700000000);
    CHECK(read.gameVersion == "0.1.0");
}

TEST_CASE("Worlds from before the Pre-map option keep the whole horizon") {
    const fs::path saves = freshSaves("old_premap");
    fs::create_directories(saves / "old");
    std::ofstream(saves / "old" / "world.toml") << "name = \"Old\"\nseed = \"1\"\npremap_blocks = 0\n";
    const auto loaded = loadWorldInfo(saves, "old");
    REQUIRE(loaded.hasValue());
    CHECK(loaded.value().premapBlocks == -1);
    CHECK(loaded.value().lodQuality == "High");
}

TEST_CASE("The world list sorts by last played and skips what is not a world") {
    const fs::path saves = freshSaves("scan");
    REQUIRE(saveWorldInfo(saves, world("old", "Old", 100)).hasValue());
    REQUIRE(saveWorldInfo(saves, world("new", "New", 300)).hasValue());
    REQUIRE(saveWorldInfo(saves, world("mid", "Mid", 200)).hasValue());
    fs::create_directories(saves / "demo" / "lod"); // A folder without world.toml.
    fs::create_directories(saves / "broken");
    std::ofstream(saves / "broken" / "world.toml") << "name = ";
    fs::create_directories(saves / "nameless");
    std::ofstream(saves / "nameless" / "world.toml") << "seed = \"1\"\n";
    const auto worlds = scanWorlds(saves);
    REQUIRE(worlds.size() == 3);
    CHECK(worlds[0].folder == "new");
    CHECK(worlds[1].folder == "mid");
    CHECK(worlds[2].folder == "old");
    CHECK(worlds[0].sizeBytes > 0);
    CHECK(scanWorlds(saves / "missing").empty());
}

TEST_CASE("World folders are safe and never collide") {
    const fs::path saves = freshSaves("folders");
    CHECK(uniqueWorldFolder(saves, "My World!") == "My_World");
    CHECK(uniqueWorldFolder(saves, "..\\..\\evil/name") == "evilname");
    CHECK(uniqueWorldFolder(saves, "???") == "world_");
    CHECK(uniqueWorldFolder(saves, "con") == "world_con");
    CHECK(uniqueWorldFolder(saves, "Demo") == "world_Demo");
    fs::create_directories(saves / "Taken");
    fs::create_directories(saves / "Taken_2");
    CHECK(uniqueWorldFolder(saves, "Taken") == "Taken_3");
}

TEST_CASE("Deleting only ever removes one real world") {
    const fs::path saves = freshSaves("delete");
    REQUIRE(saveWorldInfo(saves, world("keep", "Keep", 1)).hasValue());
    REQUIRE(saveWorldInfo(saves, world("gone", "Gone", 2)).hasValue());
    fs::create_directories(saves / "demo");
    CHECK(deleteWorld(saves, "gone").hasValue());
    CHECK_FALSE(fs::exists(saves / "gone"));
    CHECK(fs::exists(saves / "keep"));
    // Not a world, and names that leave the saves folder.
    CHECK_FALSE(deleteWorld(saves, "demo").hasValue());
    CHECK(fs::exists(saves / "demo"));
    CHECK_FALSE(deleteWorld(saves, "..").hasValue());
    CHECK_FALSE(deleteWorld(saves, "../keep").hasValue());
    CHECK_FALSE(deleteWorld(saves, "keep/../keep").hasValue());
    CHECK_FALSE(deleteWorld(saves, "").hasValue());
    CHECK(fs::exists(saves / "keep"));
}

TEST_CASE("player.dat round trips, and a damaged file means a fresh start") {
    const fs::path saves = freshSaves("player");
    REQUIRE(saveWorldInfo(saves, world("w", "W", 1)).hasValue());
    CHECK_FALSE(loadWorldInfo(saves, "w").value().player.has_value()); // Not played yet.

    PlayerData player;
    player.position = {1234.5, 70.25, -9876.125};
    player.yaw = 1.5F;
    player.pitch = -0.25F;
    player.flying = true;
    player.hotbarSlot = 4;
    player.hotbar[4] = "oak_log[axis=y]";
    REQUIRE(savePlayerData(saves, "w", player).hasValue());
    const auto loaded = loadWorldInfo(saves, "w").value().player;
    REQUIRE(loaded.has_value());
    CHECK(loaded->position.x == 1234.5);
    CHECK(loaded->position.z == -9876.125);
    CHECK(loaded->yaw == 1.5F);
    CHECK(loaded->flying);
    CHECK(loaded->hotbarSlot == 4);
    CHECK(loaded->hotbar[4] == "oak_log[axis=y]");

    std::ofstream(saves / "w" / "player.dat", std::ios::binary) << "junk";
    const auto damaged = loadWorldInfo(saves, "w");
    REQUIRE(damaged.hasValue()); // The world itself still opens.
    CHECK_FALSE(damaged.value().player.has_value());
}

TEST_CASE("Edited chunks come back from memory at once and from disk after a save") {
    const std::vector<BlockSource> sources = {
        {.file = "stone.toml",
         .text = "name = \"stone\"\ndisplay_name = \"Stone\"\nlayer = \"opaque\"\n[textures]\nall = \"s\""},
        {.file = "dirt.toml",
         .text = "name = \"dirt\"\ndisplay_name = \"Dirt\"\nlayer = \"opaque\"\n[textures]\nall = \"d\""},
    };
    const auto catalog = BlockCatalog::load(sources);
    REQUIRE(catalog.hasValue());
    const CatalogStateNames names(catalog.value());
    const auto stone = *catalog.value().stateId("stone");
    const auto dirt = *catalog.value().stateId("dirt");

    const fs::path folder = freshSaves("saver") / "regions";
    vxe::jobs::JobSystem jobs;
    vxe::config::SettingsRegistry settings;
    const WorldSaverDesc desc{.folder = folder, .jobs = &jobs, .settings = &settings, .names = &names};
    const vxe::ChunkPos position{.x = -3, .y = 2, .z = 40};
    const vxe::BlockPos block{.x = -3 * 16 + 5, .y = (2 * 16) + 6, .z = (40 * 16) + 7};
    {
        WorldSaver saver(desc);
        vxe::voxel::ChunkProduct product;
        CHECK_FALSE(saver.load(position, product)); // Never edited: the generator makes it.

        vxe::voxel::VoxelWorld voxels;
        voxels.insertChunk(position);
        REQUIRE(voxels.setBlock(block, stone));
        saver.markEdited(position);
        saver.markEdited(position); // Twice is still one chunk.
        saver.encodeEdited(voxels);
        CHECK(saver.unsavedChunks() == 1);

        // Unloaded before any save: memory has it.
        voxels.removeChunk(position);
        REQUIRE(saver.load(position, product));
        CHECK(product.origin == vxe::voxel::ChunkOrigin::Loaded);
        CHECK(product.blocks.get(vxe::toLocal(block)) == stone);

        // A newer edit wins over an older commit.
        saver.flush();
        CHECK(saver.unsavedChunks() == 0);
        voxels.insertChunk(position);
        REQUIRE(voxels.setBlock(block, dirt));
        saver.markEdited(position);
        saver.encodeEdited(voxels);
        REQUIRE(saver.load(position, product));
        CHECK(product.blocks.get(vxe::toLocal(block)) == dirt);
        saver.flush();
    }
    // A later session reads it from the region files.
    WorldSaver saver(desc);
    CHECK(saver.unsavedChunks() == 0);
    vxe::voxel::ChunkProduct product;
    REQUIRE(saver.load(position, product));
    CHECK(product.blocks.get(vxe::toLocal(block)) == dirt);
}

TEST_CASE("A world exported to a zip imports as a new world, without its far terrain cache") {
    const fs::path saves = freshSaves("zip_round_trip");
    WorldInfo info = world("my_world", "My World", 100);
    info.seedText = "42";
    info.seed = 42;
    REQUIRE(saveWorldInfo(saves, info).hasValue());
    fs::create_directories(saves / "my_world" / "regions");
    std::ofstream(saves / "my_world" / "regions" / "r.0.0.0.vxr") << "region";
    fs::create_directories(saves / "my_world" / "lod");
    std::ofstream(saves / "my_world" / "lod" / "tile.vxl") << "cache";

    const fs::path zip = saves.parent_path() / "zip_round_trip_export" / "My World.zip";
    const auto exported = exportWorld(saves, "my_world", zip);
    REQUIRE(exported.hasValue());
    CHECK(exported.value() == 2);

    const auto imported = importWorld(saves, zip);
    REQUIRE(imported.hasValue());
    CHECK(imported.value().folder == "my_world_2");
    CHECK(imported.value().name == "My World");
    CHECK(imported.value().seed == 42);
    CHECK(fs::exists(saves / "my_world_2" / "regions" / "r.0.0.0.vxr"));
    CHECK_FALSE(fs::exists(saves / "my_world_2" / "lod"));
    // Nothing half unpacked is left, and the list shows both worlds.
    CHECK(scanWorlds(saves).size() == 2);
    for (const auto& entry : fs::directory_iterator(saves)) {
        CHECK(entry.path().filename().string().rfind(".importing", 0) != 0);
    }
}

TEST_CASE("A zip without a world imports nothing and leaves nothing") {
    const fs::path saves = freshSaves("zip_no_world");
    const fs::path source = saves.parent_path() / "zip_no_world_source";
    fs::remove_all(source);
    fs::create_directories(source);
    std::ofstream(source / "readme.txt") << "not a world";
    const fs::path zip = saves.parent_path() / "zip_no_world.zip";
    REQUIRE(vxe::io::zipFolder(source, zip, "stuff").hasValue());
    CHECK_FALSE(importWorld(saves, zip).hasValue());
    CHECK(fs::is_empty(saves));
}
