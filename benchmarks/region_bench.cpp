// Region file benchmarks (E9.8): encoding and decoding chunk payloads,
// committing a batch of chunks to a region file and reading one back.

#include "benchmarks.hpp"
#include "voxel/terrain_sample.hpp"

#include <vxe/voxel/chunk_codec.hpp>
#include <vxe/voxel/region_file.hpp>

#include <nanobench.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace vxe::bench {
namespace {

using voxel::BlockStateId;

// Names "block_<id>" for every possible state, like a game catalog would give.
class BenchNames final : public voxel::BlockStateNames {
public:
    BenchNames() {
        for (u32 id = 0; id < 256; ++id) {
            m_names.push_back("block_" + std::to_string(id));
        }
    }
    [[nodiscard]] std::string_view nameOf(BlockStateId state) const override { return m_names.at(state); }
    [[nodiscard]] std::optional<BlockStateId> idOf(std::string_view name) const override {
        // "block_" plus the number, so the lookup costs about what a hash map would.
        u32 id = 0;
        for (const char digit : name.substr(6)) {
            id = (id * 10) + static_cast<u32>(digit - '0');
        }
        return static_cast<BlockStateId>(id);
    }

private:
    std::vector<std::string> m_names;
};

voxel::ChunkStorage sampleStorage(ChunkPos position) {
    std::array<BlockStateId, kChunkVolume> states{};
    test::sampleChunk(position, states);
    voxel::ChunkStorage storage;
    storage.assign(states);
    return storage;
}

} // namespace

void runRegionBenchmarks() {
    const BenchNames names;
    const voxel::ChunkStorage surface = sampleStorage(test::sampleChunkWithBits(4));
    io::BinaryWriter writer;
    voxel::encodeChunk(surface, names, writer);
    const std::vector<std::byte> payload = writer.takeBytes();

    ankerl::nanobench::Bench bench;
    bench.title("Chunk payloads and region files (4 bit terrain surface chunk)").unit("chunk");
    bench.run("encodeChunk", [&] {
        writer.clear();
        voxel::encodeChunk(surface, names, writer);
        ankerl::nanobench::doNotOptimizeAway(writer.size());
    });
    bench.run("decodeChunk", [&] {
        auto decoded = voxel::decodeChunk(payload, names);
        ankerl::nanobench::doNotOptimizeAway(decoded.hasValue());
    });

    // A batch of 64 chunks per commit, including both syncs to disk.
    const auto folder = std::filesystem::temp_directory_path() / "vxe_benchmarks";
    std::filesystem::create_directories(folder);
    const auto path = folder / "r.0.0.0.vxr";
    std::filesystem::remove(path);
    auto file = voxel::RegionFile::open(path, true);
    if (!file) {
        return;
    }
    std::vector<voxel::RegionFile::Change> changes;
    changes.reserve(64);
    for (u16 slot = 0; slot < 64; ++slot) {
        changes.push_back({.slot = slot, .payload = payload});
    }
    bench.batch(64).minEpochIterations(20).run("RegionFile::commit, 64 chunks (zstd 3, two syncs)", [&] {
        auto committed = file.value()->commit(changes);
        ankerl::nanobench::doNotOptimizeAway(committed.hasValue());
    });
    std::vector<std::byte> read;
    bench.batch(1).run("RegionFile::read (checksum, zstd)", [&] {
        auto found = file.value()->read(17, read);
        ankerl::nanobench::doNotOptimizeAway(found.hasValue());
    });
    file.value().reset();
    std::filesystem::remove(path);
}

} // namespace vxe::bench
