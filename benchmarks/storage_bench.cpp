// Chunk storage benchmarks (E9.2, Section 24): reads, writes, decompression,
// assignment and compaction per storage mode, plus the memory of the
// synthetic terrain sample against the Section 2 targets.

#include "benchmarks.hpp"
#include "voxel/terrain_sample.hpp"

#include <vxe/voxel/chunk_storage.hpp>

#include <nanobench.h>

#include <array>
#include <cstdio>
#include <format>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace vxe::bench {
namespace {

using voxel::BlockStateId;
using voxel::ChunkStorage;
using States = std::array<BlockStateId, kChunkVolume>;

// A chunk of the terrain sample with the given index width, found by
// scanning the sample (surface chunks are 4 bit, deep ones 2 bit and so on).
ChunkStorage sampleWithBits(u32 bits) {
    States states{};
    for (i32 cy = -4; cy < 8; ++cy) {
        for (i32 cx = 0; cx < 8; ++cx) {
            test::sampleChunk({.x = cx, .y = cy, .z = 0}, states);
            ChunkStorage storage;
            storage.assign(states);
            if (storage.mode() == ChunkStorage::Mode::Palette && storage.bitsPerIndex() == bits) {
                return storage;
            }
        }
    }
    return {};
}

ChunkStorage rawChunk() {
    ChunkStorage storage;
    for (u16 i = 0; i < kChunkVolume; ++i) {
        storage.setIndex(i, static_cast<BlockStateId>(i % 1000));
    }
    return storage;
}

void reportMemory() {
    States states{};
    usize mixed = 0;
    usize mixedBytes = 0;
    usize uniform = 0;
    for (i32 cz = 0; cz < 8; ++cz) {
        for (i32 cx = 0; cx < 8; ++cx) {
            for (i32 cy = -4; cy < 8; ++cy) {
                test::sampleChunk({.x = cx, .y = cy, .z = cz}, states);
                ChunkStorage storage;
                storage.assign(states);
                if (storage.isUniform()) {
                    ++uniform;
                } else {
                    ++mixed;
                    mixedBytes += storage.dataBytes();
                }
            }
        }
    }
    const std::string line = std::format(
        "\nTerrain sample memory: {} uniform chunks at {} bytes, {} mixed chunks "
        "averaging {:.1f} bytes of block data (targets: 32 and 2048)\n\n",
        uniform, sizeof(ChunkStorage), mixed, static_cast<double>(mixedBytes) / static_cast<double>(mixed));
    std::fputs(line.c_str(), stdout);
}

} // namespace

void runStorageBenchmarks() {
    reportMemory();
    std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed): the same indices every run.
    std::vector<u16> indices(kChunkVolume);
    for (u16& index : indices) {
        index = static_cast<u16>(random() % kChunkVolume);
    }

    ankerl::nanobench::Bench reads;
    reads.title("ChunkStorage reads (4096 random get)").unit("get").batch(kChunkVolume).relative(true);
    for (const u32 bits : {2U, 4U}) {
        const ChunkStorage storage = sampleWithBits(bits);
        reads.run(std::to_string(bits) + " bit palette", [&] {
            u32 sum = 0;
            for (const u16 index : indices) {
                sum += storage.getIndex(index);
            }
            ankerl::nanobench::doNotOptimizeAway(sum);
        });
    }
    const ChunkStorage raw = rawChunk();
    reads.run("raw", [&] {
        u32 sum = 0;
        for (const u16 index : indices) {
            sum += raw.getIndex(index);
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
    });

    ankerl::nanobench::Bench writes;
    writes.title("ChunkStorage writes (4096 random set, existing states)").unit("set").batch(kChunkVolume);
    ChunkStorage surface = sampleWithBits(4);
    writes.run("4 bit palette", [&] {
        for (const u16 index : indices) {
            surface.setIndex(index, static_cast<BlockStateId>(index & 3U));
        }
        ankerl::nanobench::doNotOptimizeAway(surface);
    });

    States out{};
    ankerl::nanobench::Bench decompress;
    decompress.title("ChunkStorage::decompressTo (one chunk)").unit("chunk");
    const ChunkStorage uniform(1);
    decompress.run("uniform", [&] {
        uniform.decompressTo(out);
        ankerl::nanobench::doNotOptimizeAway(out);
    });
    for (const u32 bits : {1U, 2U, 4U}) {
        const ChunkStorage storage = sampleWithBits(bits);
        if (storage.isUniform()) {
            continue; // The sample has no chunk of this width.
        }
        decompress.run(std::to_string(bits) + " bit palette", [&] {
            storage.decompressTo(out);
            ankerl::nanobench::doNotOptimizeAway(out);
        });
    }
    decompress.run("raw", [&] {
        raw.decompressTo(out);
        ankerl::nanobench::doNotOptimizeAway(out);
    });
    // Into the inside of an 18^3 snapshot box (rows of 18, slices of 324).
    constexpr usize kSide = 18;
    std::vector<BlockStateId> box(kSide * kSide * kSide);
    const ChunkStorage surface4 = sampleWithBits(4);
    decompress.run("4 bit palette, strided into 18^3", [&] {
        surface4.decompressTo(std::span(box).subspan((kSide * kSide) + kSide + 1), kSide, kSide * kSide);
        ankerl::nanobench::doNotOptimizeAway(box);
    });

    ankerl::nanobench::Bench building;
    building.title("Building and compacting (one chunk)").unit("chunk");
    States terrain{};
    test::sampleChunk({.x = 0, .y = 3, .z = 0}, terrain); // A surface chunk.
    ChunkStorage target;
    building.run("assign (terrain surface chunk)", [&] {
        target.assign(terrain);
        ankerl::nanobench::doNotOptimizeAway(target);
    });
    building.run("compact (4 bit, nothing to drop)", [&] {
        target.compact();
        ankerl::nanobench::doNotOptimizeAway(target);
    });
}

} // namespace vxe::bench
