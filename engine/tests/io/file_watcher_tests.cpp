#include <vxe/io/file_watcher.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vxe::io::FileChange;
using vxe::io::FileEvent;
using vxe::io::FileWatcher;

namespace {

std::filesystem::path makeTestFolder(std::string_view name) {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "watch" / std::string(name);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder);
    return std::filesystem::canonical(folder);
}

void writeText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream(path, std::ios::trunc) << text;
}

// Polls until at least one event about `fileName` arrived, or five seconds
// passed (file system notifications can be slow on CI machines).
bool pollUntilEventFor(FileWatcher& watcher, const std::vector<FileEvent>& events,
                       std::string_view fileName) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(watcher.poll());
        for (const FileEvent& event : events) {
            if (event.path.filename() == fileName) {
                return true;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

} // namespace

TEST_CASE("Watching a missing folder fails") {
    FileWatcher watcher;
    const auto result = watcher.watch(makeTestFolder("missing") / "nope", false, [](const FileEvent&) {});
    CHECK(result.hasError());
}

TEST_CASE("Created and modified files are reported through poll") {
    const auto folder = makeTestFolder("changes");
    FileWatcher watcher(std::chrono::milliseconds(20));
    std::vector<FileEvent> events;
    const auto id =
        watcher.watch(folder, true, [&events](const FileEvent& event) { events.push_back(event); });
    REQUIRE(id.hasValue());

    // Nothing is delivered without poll(), even when changes happened.
    writeText(folder / "stone.png", "v1");
    std::this_thread::sleep_for(50ms);
    CHECK(events.empty());

    REQUIRE(pollUntilEventFor(watcher, events, "stone.png"));
    CHECK(events.back().path == folder / "stone.png");

    // Changes in sub folders arrive for recursive watches.
    std::filesystem::create_directories(folder / "blocks");
    writeText(folder / "blocks" / "dirt.png", "v1");
    CHECK(pollUntilEventFor(watcher, events, "dirt.png"));
}

TEST_CASE("Bursts of changes to one file become a single event") {
    const auto folder = makeTestFolder("debounce");
    writeText(folder / "shader.glsl", "start");
    FileWatcher watcher(std::chrono::milliseconds(500)); // Far longer than the 50 ms burst below.
    std::vector<FileEvent> events;
    REQUIRE(watcher.watch(folder, false, [&events](const FileEvent& event) { events.push_back(event); }));
    std::this_thread::sleep_for(50ms); // Let the watch settle before changing the file.

    for (int i = 0; i < 5; ++i) {
        writeText(folder / "shader.glsl", std::to_string(i));
        std::this_thread::sleep_for(10ms);
        static_cast<void>(watcher.poll()); // Still inside the quiet period.
    }
    REQUIRE(pollUntilEventFor(watcher, events, "shader.glsl"));
    std::this_thread::sleep_for(600ms);
    static_cast<void>(watcher.poll());
    CHECK(events.size() == 1);
}

TEST_CASE("Unwatched folders stop reporting") {
    const auto folder = makeTestFolder("unwatch");
    FileWatcher watcher(std::chrono::milliseconds(10));
    std::vector<FileEvent> events;
    const auto id =
        watcher.watch(folder, false, [&events](const FileEvent& event) { events.push_back(event); });
    REQUIRE(id.hasValue());
    watcher.unwatch(id.value());
    watcher.unwatch(id.value()); // Twice is fine.

    writeText(folder / "late.txt", "x");
    std::this_thread::sleep_for(200ms);
    static_cast<void>(watcher.poll());
    CHECK(events.empty());
}

TEST_CASE("Removed files are reported as removed") {
    const auto folder = makeTestFolder("remove");
    writeText(folder / "old.toml", "x = 1");
    FileWatcher watcher(std::chrono::milliseconds(20));
    std::vector<FileEvent> events;
    REQUIRE(watcher.watch(folder, false, [&events](const FileEvent& event) { events.push_back(event); }));
    std::this_thread::sleep_for(50ms);

    std::filesystem::remove(folder / "old.toml");
    REQUIRE(pollUntilEventFor(watcher, events, "old.toml"));
    CHECK(events.back().change == FileChange::Removed);
}
