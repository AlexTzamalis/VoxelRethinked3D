#include "../test_helpers.hpp"
#include "region_test_payload.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/io/random_access_file.hpp>
#include <vxe/voxel/chunk_codec.hpp>
#include <vxe/voxel/region_file.hpp>
#include <vxe/voxel/region_store.hpp>

#include <SDL3/SDL_process.h>
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vxe;
using voxel::BlockStateId;
using voxel::RegionFile;

namespace {

std::filesystem::path freshFolder(std::string_view name) {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "region" / std::string(name);
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder);
    return folder;
}

// Names "state_<id>" for ids below `count`; anything else is unknown.
class TestNames final : public voxel::BlockStateNames {
public:
    explicit TestNames(u32 count)
        : m_count(count) {
        for (u32 id = 0; id < count; ++id) {
            m_names.push_back("state_" + std::to_string(id));
        }
    }
    [[nodiscard]] std::string_view nameOf(BlockStateId state) const override { return m_names.at(state); }
    [[nodiscard]] std::optional<BlockStateId> idOf(std::string_view name) const override {
        for (u32 id = 0; id < m_count; ++id) {
            if (m_names[id] == name) {
                return static_cast<BlockStateId>(id);
            }
        }
        return std::nullopt;
    }

private:
    u32 m_count;
    std::vector<std::string> m_names;
};

std::vector<std::byte> encode(const voxel::ChunkStorage& blocks, const voxel::BlockStateNames& names) {
    io::BinaryWriter writer;
    voxel::encodeChunk(blocks, names, writer);
    return writer.takeBytes();
}

// The (round) of every payload in a region file, by slot.
std::map<u16, u64> roundsIn(const RegionFile& file) {
    std::map<u16, u64> rounds;
    std::vector<std::byte> payload;
    for (u16 slot = 0; slot < voxel::kRegionChunks; ++slot) {
        auto found = file.read(slot, payload);
        REQUIRE(found.hasValue());
        if (found.value()) {
            const u64 round = test::regionTestRound(payload);
            CHECK(payload == test::regionTestPayload(round, slot)); // Exactly what that round wrote.
            rounds[slot] = round;
        }
    }
    return rounds;
}

Result<void> commitRound(RegionFile& file, u64 round, u16 slots) {
    std::vector<std::vector<std::byte>> payloads;
    std::vector<RegionFile::Change> changes;
    payloads.reserve(slots);
    for (u16 slot = 0; slot < slots; ++slot) {
        payloads.push_back(test::regionTestPayload(round, slot));
        changes.push_back({.slot = slot, .payload = payloads.back()});
    }
    return file.commit(changes, io::Codec::Zstd, 1);
}

} // namespace

TEST_CASE("Random access files read and write at offsets") {
    const auto folder = freshFolder("random_access");
    const auto path = folder / "data.bin";
    CHECK(io::RandomAccessFile::open(path, io::OpenMode::ReadWrite).error().code() == ErrorCode::NotFound);
    auto file = io::RandomAccessFile::open(path, io::OpenMode::ReadWriteOrCreate);
    REQUIRE(file.hasValue());
    const std::array<std::byte, 4> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    REQUIRE(file.value()->write(1000, data).hasValue()); // Past the end: the file grows.
    REQUIRE(file.value()->write(0, std::span(data).first(2)).hasValue());
    REQUIRE(file.value()->sync().hasValue());
    CHECK(file.value()->size().value() == 1004);
    std::array<std::byte, 4> back{};
    REQUIRE(file.value()->read(1000, back).hasValue());
    CHECK(back == data);
    CHECK_FALSE(file.value()->read(1002, back).hasValue()); // Ends after 2 bytes.
    file.value().reset();
    auto reopened = io::RandomAccessFile::open(path, io::OpenMode::ReadOnly);
    REQUIRE(reopened.hasValue());
    REQUIRE(reopened.value()->read(0, std::span(back).first(2)).hasValue());
    CHECK(back[1] == std::byte{2});
}

TEST_CASE("Chunk payloads round trip with names, deterministically") {
    const TestNames names(5000);
    std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed): the same chunks every run.
    const auto check = [&](const voxel::ChunkStorage& blocks) {
        const std::vector<std::byte> payload = encode(blocks, names);
        CHECK(encode(blocks, names) == payload); // Same chunk, same bytes.
        auto decoded = voxel::decodeChunk(payload, names);
        REQUIRE(decoded.hasValue());
        CHECK(decoded.value().unknownStates == 0);
        for (u16 i = 0; i < kChunkVolume; ++i) {
            REQUIRE(decoded.value().blocks.getIndex(i) == blocks.getIndex(i));
        }
        return payload.size();
    };
    voxel::ChunkStorage uniform;
    uniform.fill(3);
    CHECK(check(uniform) < 16); // Version, flags, one name.
    for (const u32 states : {2U, 3U, 17U, 300U, 4096U}) {
        std::array<BlockStateId, kChunkVolume> blocks{};
        for (usize i = 0; i < blocks.size(); ++i) {
            blocks[i] = static_cast<BlockStateId>(states == kChunkVolume ? i : random() % states);
        }
        voxel::ChunkStorage storage;
        storage.assign(blocks);
        MESSAGE(states << " states: " << check(storage) << " bytes");
    }
}

TEST_CASE("Unknown names load as the fallback; malformed payloads fail cleanly") {
    const TestNames writer(10);
    const TestNames reader(5); // States 5 to 9 were removed from the game.
    std::array<BlockStateId, kChunkVolume> blocks{};
    for (usize i = 0; i < blocks.size(); ++i) {
        blocks[i] = static_cast<BlockStateId>(i % 10);
    }
    voxel::ChunkStorage storage;
    storage.assign(blocks);
    const std::vector<std::byte> payload = encode(storage, writer);
    auto decoded = voxel::decodeChunk(payload, reader, 1);
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value().unknownStates == 5);
    CHECK(decoded.value().blocks.getIndex(7) == 1);
    CHECK(decoded.value().blocks.getIndex(3) == 3);

    // Every truncation fails, never crashes.
    for (usize size = 0; size < payload.size(); size += 7) {
        CHECK_FALSE(voxel::decodeChunk(std::span(payload).first(size), reader).hasValue());
    }
    std::vector<std::byte> bad = payload;
    bad[0] = std::byte{99};
    CHECK(voxel::decodeChunk(bad, reader).error().code() == ErrorCode::VersionMismatch);
    bad = payload;
    bad.back() = std::byte{0xFF}; // The last packed indices now point past the palette.
    CHECK_FALSE(voxel::decodeChunk(bad, reader).hasValue());
    bad = payload;
    bad.push_back(std::byte{0});
    CHECK_FALSE(voxel::decodeChunk(bad, reader).hasValue());
}

TEST_CASE("Region positions and slots, also below zero") {
    CHECK(voxel::regionOf({.x = 15, .y = 16, .z = -1}) == voxel::RegionPos{.x = 0, .y = 1, .z = -1});
    CHECK(voxel::regionOf({.x = -17, .y = 0, .z = 0}).x == -2);
    CHECK(voxel::regionSlot({.x = -1, .y = -1, .z = -1}) == 4095);
    CHECK(voxel::regionSlot({.x = 16, .y = 32, .z = 48}) == 0);
    CHECK(voxel::regionSlot({.x = 1, .y = 2, .z = 3}) == (2 * 256) + (3 * 16) + 1);
    CHECK(voxel::RegionStore::fileName({.x = -1, .y = 0, .z = 2}) == "r.-1.0.2.vxr");
}

TEST_CASE("Region files commit, reopen, replace, remove and reuse space") {
    const auto path = freshFolder("region_file") / "r.0.0.0.vxr";
    CHECK(RegionFile::open(path, false).error().code() == ErrorCode::NotFound);
    {
        auto file = RegionFile::open(path, true);
        REQUIRE(file.hasValue());
        CHECK(file.value()->chunkCount() == 0);
        CHECK(file.value()->sequence() == 0);
        REQUIRE(commitRound(*file.value(), 1, 64).hasValue());
        CHECK(file.value()->sequence() == 1);
        CHECK(file.value()->chunkCount() == 64);
    }
    auto file = RegionFile::open(path, false);
    REQUIRE(file.hasValue());
    RegionFile& region = *file.value();
    CHECK(region.sequence() == 1);
    CHECK(roundsIn(region).size() == 64);

    // Replace some, remove one, add one far away.
    const std::vector<std::byte> newer = test::regionTestPayload(7, 3);
    const std::vector<std::byte> far = test::regionTestPayload(7, 4000);
    const std::array<RegionFile::Change, 3> changes = {
        {{.slot = 3, .payload = newer}, {.slot = 10, .payload = {}}, {.slot = 4000, .payload = far}}};
    REQUIRE(region.commit(changes).hasValue());
    std::map<u16, u64> rounds = roundsIn(region);
    CHECK(rounds.size() == 64);
    CHECK(rounds.at(3) == 7);
    CHECK_FALSE(rounds.contains(10));
    CHECK(rounds.at(4000) == 7);
    CHECK(rounds.at(4) == 1);

    // Rewriting everything many times reuses freed sectors: the file stays
    // near twice the live data (the last commit plus the one being written).
    for (u64 round = 2; round < 60; ++round) {
        REQUIRE(commitRound(region, round, 64).hasValue());
    }
    const u64 bytes = std::filesystem::file_size(path);
    MESSAGE("After 60 commits: " << bytes << " bytes, " << region.usedSectors() << " sectors in use");
    CHECK(bytes < u64{3} * region.usedSectors() * RegionFile::kSectorBytes);
}

TEST_CASE("A crash at any byte of a commit leaves the old or the new commit, nothing else") {
    const auto folder = freshFolder("torn");
    const auto base = folder / "base.vxr";
    {
        auto file = RegionFile::open(base, true);
        REQUIRE(file.hasValue());
        REQUIRE(commitRound(*file.value(), 1, 40).hasValue());
    }
    // How many bytes the second commit writes, measured on a copy.
    const auto measure = folder / "measure.vxr";
    std::filesystem::copy_file(base, measure);
    u64 commitBytes = 0;
    {
        auto file = RegionFile::open(measure, false);
        REQUIRE(file.hasValue());
        REQUIRE(commitRound(*file.value(), 2, 40).hasValue());
        commitBytes = file.value()->bytesWritten();
    }
    const u64 superblockStart = commitBytes - RegionFile::kSectorBytes;
    usize sawOld = 0;
    usize sawNew = 0;
    for (u64 at = 0; at < commitBytes;) {
        const auto path = folder / "crash.vxr";
        std::filesystem::copy_file(base, path, std::filesystem::copy_options::overwrite_existing);
        {
            auto file = RegionFile::open(path, false);
            REQUIRE(file.hasValue());
            file.value()->failWritesAfter(at);
            CHECK_FALSE(commitRound(*file.value(), 2, 40).hasValue());
            // After the failure the file object matches the disk again.
            CHECK(file.value()->sequence() <= 2);
        }
        auto reopened = RegionFile::open(path, false);
        REQUIRE(reopened.hasValue());
        const std::map<u16, u64> rounds = roundsIn(*reopened.value());
        REQUIRE(rounds.size() == 40);
        const u64 round = rounds.begin()->second;
        for (const auto& [slot, value] : rounds) {
            REQUIRE(value == round); // All of one commit.
        }
        if (at < superblockStart) {
            CHECK(round == 1);
        }
        (round == 1 ? sawOld : sawNew) += 1;
        // Before the superblock, step quickly; through it, byte by byte.
        at += at + 97 < superblockStart ? 97 : 1;
    }
    MESSAGE("Crash points: " << sawOld << " kept the old commit, " << sawNew << " had the new one complete");
    CHECK(sawOld > 0);
}

TEST_CASE("Damaged superblocks fall back to the older commit; damaged chunks report errors") {
    const auto path = freshFolder("damaged") / "r.0.0.0.vxr";
    {
        auto file = RegionFile::open(path, true);
        REQUIRE(file.hasValue());
        REQUIRE(commitRound(*file.value(), 1, 8).hasValue()); // Superblock slot 1.
        REQUIRE(commitRound(*file.value(), 2, 8).hasValue()); // Slot 0.
    }
    const auto scribble = [&path](u64 offset, usize bytes) {
        auto raw = io::RandomAccessFile::open(path, io::OpenMode::ReadWrite);
        REQUIRE(raw.hasValue());
        const std::vector<std::byte> junk(bytes, std::byte{0x5A});
        REQUIRE(raw.value()->write(offset, junk).hasValue());
    };
    scribble(8, 4); // Inside the newest superblock (slot 0).
    {
        auto file = RegionFile::open(path, false);
        REQUIRE(file.hasValue());
        CHECK(file.value()->sequence() == 1);
        CHECK(roundsIn(*file.value()).begin()->second == 1);
    }
    scribble(RegionFile::kSectorBytes + 8, 4); // Now the other one too.
    CHECK(RegionFile::open(path, false).error().code() == ErrorCode::ParseError);

    // A damaged payload is caught by its checksum.
    const auto second = freshFolder("damaged_payload") / "r.0.0.0.vxr";
    {
        auto file = RegionFile::open(second, true);
        REQUIRE(file.hasValue());
        REQUIRE(commitRound(*file.value(), 1, 1).hasValue());
    }
    auto raw = io::RandomAccessFile::open(second, io::OpenMode::ReadWrite);
    REQUIRE(raw.hasValue());
    const std::array<std::byte, 1> junk{std::byte{0x77}};
    REQUIRE(
        raw.value()->write(u64{2} * RegionFile::kSectorBytes + 5, junk).hasValue()); // The first data sector.
    raw.value().reset();
    auto file = RegionFile::open(second, false);
    REQUIRE(file.hasValue());
    std::vector<std::byte> payload;
    CHECK(file.value()->read(0, payload).error().code() == ErrorCode::ParseError);
}

TEST_CASE("Reads from many threads during commits always see whole commits") {
    const auto path = freshFolder("concurrent") / "r.0.0.0.vxr";
    auto opened = RegionFile::open(path, true);
    REQUIRE(opened.hasValue());
    RegionFile& region = *opened.value();
    REQUIRE(commitRound(region, 1, 32).hasValue());
    std::atomic<bool> stop{false};
    std::atomic<u32> bad{0};
    std::vector<std::thread> readers;
    readers.reserve(4);
    for (u32 t = 0; t < 4; ++t) {
        readers.emplace_back([&region, &stop, &bad, t] {
            std::vector<std::byte> payload;
            u16 slot = static_cast<u16>(t);
            while (!stop.load()) {
                auto found = region.read(slot, payload);
                if (!found || !found.value() ||
                    payload != test::regionTestPayload(test::regionTestRound(payload), slot)) {
                    ++bad;
                }
                slot = static_cast<u16>((slot + 5) % 32);
            }
        });
    }
    for (u64 round = 2; round < 80; ++round) {
        REQUIRE(commitRound(region, round, 32).hasValue());
    }
    stop = true;
    for (std::thread& reader : readers) {
        reader.join();
    }
    CHECK(bad.load() == 0);
}

TEST_CASE("Killing a process in the middle of commits never breaks the region file") {
    const auto path = freshFolder("kill") / "r.0.0.0.vxr";
    const std::string file = path.string();
    std::mt19937 random(11); // NOLINT(bugprone-random-generator-seed): repeatable kill times.
    u64 lastRound = 0;
    usize kills = 0;
    for (u32 run = 0; run < 12; ++run) {
        const std::array<const char*, 3> arguments = {VXE_REGION_CRASH_WRITER, file.c_str(), nullptr};
        SDL_Process* process = SDL_CreateProcess(arguments.data(), false);
        REQUIRE(process != nullptr);
        // Process start up takes 0.1 to 0.3 s (more with the profiler built
        // in), so the random kill time counts from the file appearing.
        for (u32 wait = 0; wait < 500 && !std::filesystem::exists(path); ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(random() % 200));
        int exitCode = 0;
        if (!SDL_WaitProcess(process, false, &exitCode)) {
            CHECK(SDL_KillProcess(process, true));
            ++kills;
        }
        SDL_WaitProcess(process, true, &exitCode);
        SDL_DestroyProcess(process);

        auto opened = RegionFile::open(path, false);
        if (!opened) {
            MESSAGE(opened.error().describe());
        }
        REQUIRE(opened.hasValue()); // The file always opens.
        const std::map<u16, u64> rounds = roundsIn(*opened.value());
        if (rounds.empty()) {
            CHECK(opened.value()->sequence() == 0); // Killed before its first commit was complete.
            continue;
        }
        REQUIRE(rounds.size() == 64);
        const u64 round = rounds.begin()->second;
        for (const auto& [slot, value] : rounds) {
            REQUIRE(value == round); // One whole commit.
        }
        CHECK(round == opened.value()->sequence());
        CHECK(round >= lastRound); // Later runs continue the file.
        lastRound = round;
    }
    MESSAGE("Killed the writer " << kills << " times; the file reached commit " << lastRound);
    CHECK(kills > 0);
    CHECK(lastRound > 0);
}

TEST_CASE("The region store saves and loads across regions and keeps few files open") {
    const auto folder = freshFolder("store");
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    voxel::RegionStore store({.folder = folder, .settings = &settings, .stats = &stats});
    REQUIRE(settings.set("Server.Save.MaxOpenRegionFiles", config::SettingValue{4}).hasValue());

    std::vector<std::byte> payload;
    REQUIRE(store.load({.x = 3, .y = 3, .z = 3}, payload).hasValue());
    CHECK_FALSE(store.load({.x = 3, .y = 3, .z = 3}, payload).value());
    CHECK(std::filesystem::is_empty(folder)); // Reading never creates files.

    // 10 regions, a few chunks each, some below zero.
    std::vector<std::vector<std::byte>> payloads;
    std::vector<voxel::RegionStore::SavedChunk> chunks;
    std::vector<ChunkPos> positions;
    for (i32 region = -5; region < 5; ++region) {
        for (i32 k = 0; k < 3; ++k) {
            positions.push_back({.x = (region * 16) + k, .y = -k, .z = 7});
        }
    }
    payloads.reserve(positions.size());
    for (usize i = 0; i < positions.size(); ++i) {
        payloads.push_back(test::regionTestPayload(i + 1, voxel::regionSlot(positions[i])));
        chunks.push_back({.position = positions[i], .payload = payloads.back()});
    }
    REQUIRE(store.save(chunks).hasValue());
    CHECK(store.openFiles() <= 4);
    stats.endFrame(1.0 / 60.0);
    CHECK(stats.value(stats.find("Voxel.Regions.ChunksWritten")) == doctest::Approx(30.0));
    CHECK(stats.value(stats.find("Voxel.Regions.Commits")) >= 10.0);
    for (usize i = 0; i < positions.size(); ++i) {
        auto found = store.load(positions[i], payload);
        REQUIRE(found.hasValue());
        REQUIRE(found.value());
        CHECK(payload == payloads[i]);
    }
    CHECK(store.openFiles() <= 4);

    // Removing a chunk.
    const std::array<voxel::RegionStore::SavedChunk, 1> removal = {
        {{.position = positions[0], .payload = {}}}};
    REQUIRE(store.save(removal).hasValue());
    CHECK_FALSE(store.load(positions[0], payload).value());
    store.closeAll();
    CHECK(store.openFiles() == 0);
    CHECK(store.load(positions[1], payload).value()); // Opens again on demand.
}
