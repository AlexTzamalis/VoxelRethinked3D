#include "vxe/voxel/chunk_codec.hpp"

#include <vxe/core/profile.hpp>

#include <array>
#include <bit>
#include <format>
#include <vector>

namespace vxe::voxel {
namespace {

constexpr u32 kMaxPalette = kChunkVolume;
constexpr u16 kNotInPalette = 0xFFFF;

// Bits needed to store the values 0 to count - 1 (at least 1).
constexpr u32 indexBits(u32 count) noexcept {
    return count <= 2 ? 1U : static_cast<u32>(std::bit_width(count - 1));
}

// Scratch memory per thread, so saving and loading chunks allocates only the
// output. The remap table has an entry per possible state id (128 KB) and is
// put back to "not in palette" after every chunk.
struct Scratch {
    std::array<BlockStateId, kChunkVolume> states{};
    std::array<u16, kChunkVolume> indices{};
    std::vector<u16> remap = std::vector<u16>(kMaxBlockStates, kNotInPalette);
    std::vector<BlockStateId> palette;
};

Scratch& scratch() {
    thread_local Scratch instance;
    return instance;
}

} // namespace

void encodeChunk(const ChunkStorage& blocks, const BlockStateNames& names, io::BinaryWriter& out) {
    VXE_PROFILE_FUNCTION();
    out.writeU8(kChunkPayloadVersion);
    out.writeU8(0);
    if (blocks.isUniform()) {
        out.writeVarU64(1);
        out.writeString(names.nameOf(blocks.uniformState()));
        return;
    }
    Scratch& work = scratch();
    blocks.decompressTo(work.states);
    work.palette.clear();
    for (usize i = 0; i < kChunkVolume; ++i) {
        const BlockStateId state = work.states[i];
        u16& slot = work.remap[state];
        if (slot == kNotInPalette) {
            slot = static_cast<u16>(work.palette.size());
            work.palette.push_back(state);
        }
        work.indices[i] = slot;
    }
    const auto count = static_cast<u32>(work.palette.size());
    out.writeVarU64(count);
    for (const BlockStateId state : work.palette) {
        out.writeString(names.nameOf(state));
        work.remap[state] = kNotInPalette;
    }
    if (count == 1) {
        return;
    }
    const u32 bits = indexBits(count);
    const u32 perWord = 64U / bits;
    out.writeU8(static_cast<u8>(bits));
    for (usize first = 0; first < kChunkVolume; first += perWord) {
        u64 word = 0;
        for (u32 k = 0; k < perWord && first + k < kChunkVolume; ++k) {
            word |= u64{work.indices[first + k]} << (k * bits);
        }
        out.writeU64(word);
    }
}

Result<DecodedChunk> decodeChunk(std::span<const std::byte> payload, const BlockStateNames& names,
                                 BlockStateId fallback) {
    VXE_PROFILE_FUNCTION();
    io::BinaryReader reader(payload);
    const u8 version = reader.readU8();
    const u8 flags = reader.readU8();
    if (!reader.ok()) {
        return Error(ErrorCode::ParseError, "Chunk payload is empty or truncated");
    }
    if (version != kChunkPayloadVersion) {
        return Error(ErrorCode::VersionMismatch,
                     std::format("Chunk payload version {} is not supported (this build reads {})", version,
                                 kChunkPayloadVersion));
    }
    if (flags != 0) {
        return Error(ErrorCode::VersionMismatch, std::format("Chunk payload has unknown flags {:#x}", flags));
    }
    const u64 count = reader.readVarU64();
    if (!reader.ok() || count == 0 || count > kMaxPalette) {
        return Error(ErrorCode::ParseError, std::format("Chunk payload has a palette of {} states", count));
    }
    DecodedChunk result;
    Scratch& work = scratch();
    work.palette.clear();
    for (u64 i = 0; i < count; ++i) {
        const std::string name = reader.readString();
        if (!reader.ok()) {
            return Error(ErrorCode::ParseError, "Chunk payload palette is truncated");
        }
        const std::optional<BlockStateId> state = names.idOf(name);
        if (!state) {
            ++result.unknownStates;
        }
        work.palette.push_back(state.value_or(fallback));
    }
    if (count == 1) {
        if (!reader.atEnd()) {
            return Error(ErrorCode::ParseError, "Chunk payload has data after a one state palette");
        }
        result.blocks.fill(work.palette.front());
        return result;
    }
    const u32 bits = reader.readU8();
    if (bits != indexBits(static_cast<u32>(count))) {
        return Error(ErrorCode::ParseError,
                     std::format("Chunk payload uses {} bit indices for {} states", bits, count));
    }
    const u32 perWord = 64U / bits;
    const u64 mask = (u64{1} << bits) - 1;
    for (usize first = 0; first < kChunkVolume; first += perWord) {
        const u64 word = reader.readU64();
        for (u32 k = 0; k < perWord && first + k < kChunkVolume; ++k) {
            const u64 index = (word >> (k * bits)) & mask;
            if (index >= count) {
                return Error(ErrorCode::ParseError, "Chunk payload has an index outside its palette");
            }
            work.states[first + k] = work.palette[static_cast<usize>(index)];
        }
    }
    if (!reader.ok() || !reader.atEnd()) {
        return Error(
            ErrorCode::ParseError,
            std::format("Chunk payload of {} bytes is truncated or has extra bytes", payload.size()));
    }
    result.blocks.assign(work.states);
    return result;
}

} // namespace vxe::voxel
