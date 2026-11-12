#include <vxe/io/file_io.hpp>
#include <vxe/io/zip.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace vxe;

namespace {

fs::path freshFolder(const char* name) {
    const fs::path folder = fs::temp_directory_path() / "vxe_tests" / "zip" / name;
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder);
    return folder;
}

void writeText(const fs::path& path, std::string_view text) {
    REQUIRE(io::writeFile(path, std::as_bytes(std::span(text))).hasValue());
}

std::string readText(const fs::path& path) {
    const auto bytes = io::readFile(path);
    REQUIRE(bytes.hasValue());
    return {reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()};
}

} // namespace

TEST_CASE("A folder zips and unzips under its root name, skipping cache folders") {
    const fs::path root = freshFolder("round_trip");
    const fs::path world = root / u8"W\u00f6rld";
    writeText(world / "world.toml", "name = \"W\"");
    writeText(world / "regions" / "r.0.0.0.vxr", std::string(70000, 'x'));
    writeText(world / "lod" / "l1" / "t.0.0.vxl", "cache");
    constexpr std::array<std::string_view, 1> kSkip = {"lod"};
    const auto packed = io::zipFolder(world, root / "out" / "world.zip", "My World", kSkip);
    REQUIRE(packed.hasValue());
    CHECK(packed.value() == 2);
    CHECK(fs::file_size(root / "out" / "world.zip") < 2000); // 70,000 equal bytes compress well.

    const auto unpacked = io::unzipTo(root / "out" / "world.zip", root / "back");
    REQUIRE(unpacked.hasValue());
    CHECK(unpacked.value() == 2);
    CHECK(readText(root / "back" / "My World" / "world.toml") == "name = \"W\"");
    CHECK(readText(root / "back" / "My World" / "regions" / "r.0.0.0.vxr").size() == 70000);
    CHECK_FALSE(fs::exists(root / "back" / "My World" / "lod"));
}

TEST_CASE("Zip entries can never point outside the destination") {
    CHECK(io::safeZipEntryPath("World/world.toml") == fs::path("World") / "world.toml");
    CHECK(io::safeZipEntryPath("World\\regions\\r.vxr") == fs::path("World") / "regions" / "r.vxr");
    CHECK(io::safeZipEntryPath("./a//b") == fs::path("a") / "b");
    CHECK(io::safeZipEntryPath("../evil.txt").empty());
    CHECK(io::safeZipEntryPath("World/../../evil.txt").empty());
    CHECK(io::safeZipEntryPath("/etc/passwd").empty());
    CHECK(io::safeZipEntryPath("C:/Windows/evil.dll").empty());
    CHECK(io::safeZipEntryPath("").empty());
}

TEST_CASE("Unzipping something that is not a zip fails cleanly") {
    const fs::path root = freshFolder("broken");
    writeText(root / "fake.zip", "not a zip at all");
    CHECK_FALSE(io::unzipTo(root / "fake.zip", root / "out").hasValue());
    CHECK_FALSE(io::unzipTo(root / "missing.zip", root / "out").hasValue());
}
