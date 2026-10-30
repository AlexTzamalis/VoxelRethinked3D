#include "server/world_store.hpp"

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
