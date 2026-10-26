#include <vxe/core/log.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace {

std::string readFile(const std::filesystem::path& path) {
    const std::ifstream file(path);
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// A fresh folder per run so an old log file can never make a test pass.
std::filesystem::path makeTestFolder() {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "log";
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder);
    return folder;
}

} // namespace

TEST_CASE("Category levels filter messages") {
    vxe::log::Category category{"test.filter", vxe::log::Level::Warn};
    CHECK_FALSE(category.shouldLog(vxe::log::Level::Info));
    CHECK(category.shouldLog(vxe::log::Level::Warn));
    CHECK(category.shouldLog(vxe::log::Level::Critical));

    category.setLevel(vxe::log::Level::Trace);
    CHECK(category.shouldLog(vxe::log::Level::Trace));

    category.setLevel(vxe::log::Level::Off);
    CHECK_FALSE(category.shouldLog(vxe::log::Level::Critical));
}

TEST_CASE("Categories are registered once and found by name") {
    static vxe::log::Category category{"test.registry", vxe::log::Level::Info};
    vxe::log::registerCategory(category);
    vxe::log::registerCategory(category);

    CHECK(vxe::log::findCategory("test.registry") == &category);
    CHECK(vxe::log::findCategory("does.not.exist") == nullptr);

    int matches = 0;
    for (const vxe::log::Category* registered : vxe::log::categories()) {
        matches += registered == &category ? 1 : 0;
    }
    CHECK(matches == 1);
}

TEST_CASE("Level names") {
    CHECK(vxe::log::toString(vxe::log::Level::Info) == "info");
    CHECK(vxe::log::toString(vxe::log::Level::Critical) == "critical");
}

TEST_CASE("initialize creates the log file and writes formatted messages") {
    const auto logFile = makeTestFolder() / "nested" / "latest.log";

    vxe::log::Config config;
    config.filePath = logFile;
    config.console = false;
    REQUIRE(vxe::log::initialize(config));
    CHECK(vxe::log::findCategory("core") == &vxe::log::core);

    const vxe::log::Level previous = vxe::log::core.level();
    vxe::log::core.setLevel(vxe::log::Level::Info);
    VXE_LOG_INFO(vxe::log::core, "hello {} from {}", 42, "the tests");
    VXE_LOG_DEBUG(vxe::log::core, "this debug message is filtered");
    vxe::log::core.setLevel(previous);
    vxe::log::shutdown();

    REQUIRE(std::filesystem::exists(logFile));
    const std::string contents = readFile(logFile);
    CHECK(contents.find("[core] hello 42 from the tests") != std::string::npos);
    CHECK(contents.find("filtered") == std::string::npos);
    CHECK(contents.find("info") != std::string::npos);
}

TEST_CASE("Logging before initialize or after shutdown does not crash") {
    vxe::log::shutdown();
    VXE_LOG_WARN(vxe::log::core, "goes to stderr, value {}", 7);
    vxe::log::flush();
}
