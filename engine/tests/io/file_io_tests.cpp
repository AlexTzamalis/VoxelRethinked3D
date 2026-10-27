#include <vxe/io/file_io.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vxe::ErrorCode;
using vxe::io::AsyncFileIo;
using vxe::io::Completion;
using vxe::io::WriteMode;
using vxe::jobs::JobSystem;
using vxe::jobs::JobSystemConfig;

namespace {

std::filesystem::path makeTestFolder(std::string_view name) {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "file_io" / std::string(name);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder);
    return folder;
}

std::vector<std::byte> bytesOf(std::string_view text) {
    const auto view = std::as_bytes(std::span(text.data(), text.size()));
    return {view.begin(), view.end()};
}

// Runs main thread tasks until `done` is true, failing after two seconds.
void pumpUntil(JobSystem& jobs, const bool& done) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(jobs.runMainThreadTasks());
        std::this_thread::sleep_for(100us);
    }
    REQUIRE(done);
}

} // namespace

TEST_CASE("readFile and writeFile round trip, creating folders") {
    const auto folder = makeTestFolder("sync");
    const auto path = folder / "nested" / "deeper" / "data.bin";
    REQUIRE(vxe::io::writeFile(path, bytesOf("hello voxels")).hasValue());
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp")); // Atomic write cleaned up.

    const auto read = vxe::io::readFile(path);
    REQUIRE(read.hasValue());
    CHECK(read.value() == bytesOf("hello voxels"));

    // Atomic writes replace an existing file completely.
    REQUIRE(vxe::io::writeFile(path, bytesOf("v2")).hasValue());
    CHECK(vxe::io::readFile(path).value() == bytesOf("v2"));

    REQUIRE(vxe::io::writeFile(folder / "direct.log", bytesOf("x"), WriteMode::Direct).hasValue());
    CHECK(vxe::io::readFile(folder / "direct.log").value() == bytesOf("x"));

    REQUIRE(vxe::io::writeFile(folder / "empty.bin", {}).hasValue());
    CHECK(vxe::io::readFile(folder / "empty.bin").value().empty());
}

TEST_CASE("Reading a missing file returns NotFound") {
    const auto result = vxe::io::readFile(makeTestFolder("missing") / "nope.bin");
    REQUIRE(result.hasError());
    CHECK(result.error().code() == ErrorCode::NotFound);
}

TEST_CASE("Async reads deliver on the main thread") {
    const auto folder = makeTestFolder("async_read");
    REQUIRE(vxe::io::writeFile(folder / "a.bin", bytesOf("async")).hasValue());

    JobSystem jobs(JobSystemConfig{.workerCount = 2, .ioThreadCount = 1});
    AsyncFileIo files(jobs);
    const std::thread::id mainThread = std::this_thread::get_id();

    bool done = false;
    bool onMainThread = false;
    std::vector<std::byte> contents;
    files.read(folder / "a.bin", [&](vxe::Result<std::vector<std::byte>>&& result) {
        onMainThread = std::this_thread::get_id() == mainThread;
        if (result) {
            contents = std::move(result).value();
        }
        done = true;
    });
    pumpUntil(jobs, done);
    CHECK(onMainThread);
    CHECK(contents == bytesOf("async"));
}

TEST_CASE("Async writes complete on workers, and errors are delivered too") {
    const auto folder = makeTestFolder("async_write");
    JobSystem jobs(JobSystemConfig{.workerCount = 2, .ioThreadCount = 1});
    AsyncFileIo files(jobs);

    std::atomic<bool> written{false};
    std::atomic<bool> ranOnWorker{false};
    files.write(
        folder / "saved.bin", bytesOf("saved data"),
        [&](vxe::Result<void>&& result) {
            const vxe::Result<void> outcome = std::move(result);
            ranOnWorker.store(jobs.currentWorkerIndex().has_value());
            written.store(outcome.hasValue());
        },
        Completion::Worker);

    std::atomic<bool> failed{false};
    std::atomic<bool> errorWasNotFound{false};
    files.read(
        folder / "never_written.bin",
        [&](vxe::Result<std::vector<std::byte>>&& result) {
            const vxe::Result<std::vector<std::byte>> outcome = std::move(result);
            errorWasNotFound.store(outcome.hasError() && outcome.error().code() == ErrorCode::NotFound);
            failed.store(true);
        },
        Completion::IoThread);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((!written.load() || !failed.load()) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100us);
    }
    REQUIRE(written.load());
    REQUIRE(failed.load());
    CHECK(ranOnWorker.load());
    CHECK(errorWasNotFound.load());
    CHECK(vxe::io::readFile(folder / "saved.bin").value() == bytesOf("saved data"));
}
