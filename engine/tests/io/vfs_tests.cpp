#include <vxe/io/vfs.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using vxe::ErrorCode;
using vxe::io::MountAccess;
using vxe::io::Vfs;

namespace {

// A fresh temporary folder per test case.
std::filesystem::path makeTestFolder(std::string_view name) {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "vfs" / std::string(name);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder);
    return folder;
}

std::string normalized(std::string_view path) {
    const auto result = vxe::io::normalizeRelativePath(path);
    REQUIRE(result.hasValue());
    return result.value();
}

bool rejected(std::string_view path) {
    const auto result = vxe::io::normalizeRelativePath(path);
    return result.hasError() && result.error().code() == ErrorCode::InvalidArgument;
}

} // namespace

TEST_CASE("Relative paths are normalized") {
    CHECK(normalized("a/b/c.txt") == "a/b/c.txt");
    CHECK(normalized("a/./b//c") == "a/b/c");
    CHECK(normalized("a\\b\\c") == "a/b/c");
    CHECK(normalized("a/../b") == "b");
    CHECK(normalized("a/b/../../c") == "c");
    CHECK(normalized("").empty());
    CHECK(normalized("./").empty());
    CHECK(normalized("My World/region 1.vxr") == "My World/region 1.vxr");
    CHECK(normalized("names.with.dots/v1.2.toml") == "names.with.dots/v1.2.toml");
}

TEST_CASE("Paths that could escape or alias are rejected") {
    CHECK(rejected("../secret.txt"));
    CHECK(rejected("a/../../secret.txt"));
    CHECK(rejected("/etc/passwd"));
    CHECK(rejected("\\\\server\\share"));
    CHECK(rejected("C:/Windows/system32"));
    CHECK(rejected("file.txt:stream")); // NTFS alternate data stream.
    CHECK(rejected("con"));
    CHECK(rejected("saves/NUL.txt"));
    CHECK(rejected("COM1"));
    CHECK(rejected("lpt9.log"));
    CHECK(rejected("world."));
    CHECK(rejected("world "));
    CHECK(rejected("a\tb"));
    CHECK(rejected("what?.txt"));
    CHECK(rejected("pipe|name"));

    // Look alikes of device names are fine.
    CHECK(normalized("console.txt") == "console.txt");
    CHECK(normalized("com10") == "com10");
}

TEST_CASE("Mounts resolve virtual paths inside their root") {
    const auto root = makeTestFolder("resolve");
    Vfs vfs;
    REQUIRE(vfs.mount("assets", root).hasValue());
    CHECK(vfs.isMounted("assets"));

    const auto file = vfs.resolve("assets:/data/blocks/stone.toml");
    REQUIRE(file.hasValue());
    CHECK(file.value() ==
          std::filesystem::absolute(root).lexically_normal() / "data" / "blocks" / "stone.toml");

    const auto rootPath = vfs.resolve("assets:/");
    REQUIRE(rootPath.hasValue());
    CHECK(rootPath.value() == std::filesystem::absolute(root).lexically_normal());

    // Escapes fail before any file system access.
    CHECK(vfs.resolve("assets:/../outside.txt").error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("Unknown mounts and malformed virtual paths fail clearly") {
    Vfs vfs;
    CHECK(vfs.resolve("saves:/world").error().code() == ErrorCode::NotFound);
    CHECK(vfs.resolve("no-separator").error().code() == ErrorCode::InvalidArgument);
    CHECK(vfs.mount("Bad Name", std::filesystem::temp_directory_path()).error().code() ==
          ErrorCode::InvalidArgument);
    CHECK(vfs.mount("missing", makeTestFolder("missing") / "does_not_exist").error().code() ==
          ErrorCode::NotFound);
}

TEST_CASE("Read only mounts refuse writes") {
    const auto root = makeTestFolder("access");
    Vfs vfs;
    REQUIRE(vfs.mount("assets", root / "assets", {.access = MountAccess::ReadOnly, .createIfMissing = true}));
    REQUIRE(vfs.mount("saves", root / "saves", {.access = MountAccess::ReadWrite, .createIfMissing = true}));
    CHECK(std::filesystem::is_directory(root / "saves")); // Created on mount.

    CHECK(vfs.resolveForWrite("assets:/x.txt").error().code() == ErrorCode::PermissionDenied);
    CHECK(vfs.resolveForWrite("saves:/World/world.toml").hasValue());
    CHECK(vfs.resolve("assets:/x.txt").hasValue()); // Reading is still fine.
}

TEST_CASE("exists and list see the real folder") {
    const auto root = makeTestFolder("list");
    std::filesystem::create_directories(root / "b_folder");
    std::ofstream(root / "a.txt") << "hello";
    std::ofstream(root / "c.toml") << "x = 1";

    Vfs vfs;
    REQUIRE(vfs.mount("config", root, {.access = MountAccess::ReadWrite}));
    CHECK(vfs.exists("config:/a.txt"));
    CHECK_FALSE(vfs.exists("config:/missing.txt"));
    CHECK_FALSE(vfs.exists("config:/../a.txt"));

    const auto names = vfs.list("config:/");
    REQUIRE(names.hasValue());
    CHECK(names.value() == std::vector<std::string>{"a.txt", "b_folder", "c.toml"});
    CHECK(vfs.list("config:/nope").error().code() == ErrorCode::NotFound);
}

TEST_CASE("Mounts can be replaced and removed") {
    const auto first = makeTestFolder("first");
    const auto second = makeTestFolder("second");
    Vfs vfs;
    REQUIRE(vfs.mount("textures", first));
    REQUIRE(vfs.mount("textures", second));
    CHECK(vfs.resolve("textures:/").value() == std::filesystem::absolute(second).lexically_normal());
    CHECK(vfs.unmount("textures"));
    CHECK_FALSE(vfs.unmount("textures"));
    CHECK_FALSE(vfs.isMounted("textures"));
}
