#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/io/binary.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_light.hpp>
#include <vxe/voxel/chunk_storage.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace vxe::voxel {

// The names of block states, for saving. Runtime state ids may change between
// versions of the game (new blocks, reordered files), so saves store names
// (Section 9.3) and the game, which owns the block catalog, translates.
//
// Thread safety: both functions are called from save and load jobs, so they
// must be safe to call from several threads at once (read only data).
class BlockStateNames {
public:
    virtual ~BlockStateNames() = default;
    BlockStateNames() = default;
    BlockStateNames(const BlockStateNames&) = delete;
    BlockStateNames& operator=(const BlockStateNames&) = delete;
    BlockStateNames(BlockStateNames&&) = delete;
    BlockStateNames& operator=(BlockStateNames&&) = delete;

    // The saved name of a state, for example "oak_log[axis=y]".
    [[nodiscard]] virtual std::string_view nameOf(BlockStateId state) const = 0;
    // The state with a saved name, or nothing if the game no longer has it.
    [[nodiscard]] virtual std::optional<BlockStateId> idOf(std::string_view name) const = 0;
};

// The format of a chunk payload; bumped when it changes, and old versions are
// then migrated when read (Section 21).
inline constexpr u8 kChunkPayloadVersion = 1;

// Writes the payload of one chunk (Section 21) to `out`, uncompressed; the
// region file compresses it. Layout, little endian:
//   u8      payload version (kChunkPayloadVersion)
//   u8      flags: bit 0 says light follows the indices
//   varint  palette size P, 1 to 4096
//   P x     state name (varint length, then the bytes)
//   if P > 1:
//     u8    bits per index B, the bits needed for P - 1 (1 to 12)
//     u64   4096 indices into the palette in block order (y, z, x), packed
//           64 / B per word from the low bits up, never across words
//   if flag bit 0:
//     u8    0: uniform light, then one packed light byte (packLight)
//           1: 4096 packed light bytes in block order
// The palette lists only the states the chunk uses, in order of first use, so
// the same chunk always gives the same bytes. Each chunk carries its own
// names; zstd shrinks the repeats across a region's chunks far more cheaply
// than a shared region palette would, and a chunk stays readable on its own.
// With `light`, the chunk's light is saved too, so loading needs no relight.
void encodeChunk(const ChunkStorage& blocks, const BlockStateNames& names, io::BinaryWriter& out,
                 const ChunkLight* light = nullptr);

struct DecodedChunk {
    ChunkStorage blocks;
    // Palette names the game no longer knows; their blocks became `fallback`.
    u32 unknownStates = 0;
    // The saved light, if the payload has it.
    std::optional<ChunkLight> light;
};

// Reads a payload written by encodeChunk. Fails on anything malformed
// (truncated, an index outside the palette, an unknown version), never reads
// out of bounds. Names the game does not know load as `fallback`.
[[nodiscard]] Result<DecodedChunk> decodeChunk(std::span<const std::byte> payload,
                                               const BlockStateNames& names,
                                               BlockStateId fallback = kAirState);

} // namespace vxe::voxel
