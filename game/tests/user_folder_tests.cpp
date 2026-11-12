#include "app/user_folder.hpp"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;
using namespace vr;

namespace {

fs::path fresh(const char* name) {
    const fs::path folder = fs::temp_directory_path() / "vr_user_folder_tests" / name;
    fs::remove_all(folder);
    fs::create_directories(folder);
    return folder;
}

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

constexpr std::array<std::string_view, 1> kShipped = {"default"};

} // namespace

TEST_CASE("The user folder is the system's unless the program runs portable") {
    const fs::path program = fresh("choose");
    CHECK(chooseUserFolder(program, "C:/Users/A/AppData/Roaming", false) ==
          fs::path("C:/Users/A/AppData/Roaming") / ".voxelrethinked");
    CHECK(chooseUserFolder(program, "C:/Roaming", true).empty());
    CHECK(chooseUserFolder(program, {}, false).empty());
    write(program / "portable.txt", "yes");
    CHECK(chooseUserFolder(program, "C:/Roaming", false).empty());
}

TEST_CASE("Old player files are copied once into an empty user folder, originals kept") {
    const fs::path root = fresh("copy");
    const fs::path program = root / "program";
    const fs::path user = root / "user";
    write(program / "config" / "settings.toml", "a = 1");
    write(program / "saves" / "World" / "world.toml", "name = \"World\"");
    write(program / "saves" / "World" / "regions" / "r.0.0.0.vxr", "data");
    write(program / "textures" / "default" / "pack.toml", "shipped");
    write(program / "textures" / "MyPack" / "pack.toml", "mine");
    // Mounting creates the user folders empty before the copy runs.
    fs::create_directories(user / "config");
    fs::create_directories(user / "saves");

    const auto copied = copyOldUserFiles(program, user, kShipped);
    REQUIRE(copied.hasValue());
    CHECK(copied.value() == 4);
    CHECK(fs::exists(user / "config" / "settings.toml"));
    CHECK(fs::exists(user / "saves" / "World" / "regions" / "r.0.0.0.vxr"));
    CHECK(fs::exists(user / "resourcepacks" / "MyPack" / "pack.toml"));
    CHECK_FALSE(fs::exists(user / "resourcepacks" / "default"));
    CHECK(fs::exists(program / "saves" / "World" / "world.toml"));

    // A second start finds settings there and copies nothing again.
    write(program / "saves" / "Later" / "world.toml", "name = \"Later\"");
    const auto again = copyOldUserFiles(program, user, kShipped);
    REQUIRE(again.hasValue());
    CHECK(again.value() == 0);
    CHECK_FALSE(fs::exists(user / "saves" / "Later"));
}

TEST_CASE("A portable program copies nothing onto itself") {
    const fs::path program = fresh("same");
    write(program / "config" / "settings.toml", "a = 1");
    const auto copied = copyOldUserFiles(program, program, kShipped);
    REQUIRE(copied.hasValue());
    CHECK(copied.value() == 0);
}

TEST_CASE("The user folder of a config folder, with or without a trailing separator") {
    const fs::path user = fs::path("C:/Roaming") / ".voxelrethinked";
    CHECK(userFolderOf(user / "config") == user);
    CHECK(userFolderOf(fs::path("C:/Roaming/.voxelrethinked/config/")) == user);
}
