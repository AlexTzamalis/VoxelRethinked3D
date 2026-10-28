#include "vxe/voxel/chunk_storage.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/memory.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace vxe::voxel {
namespace {

constexpr usize kVolume = static_cast<usize>(kChunkVolume);
constexpr usize kRawBytes = kVolume * sizeof(BlockStateId);

// 64 bit words holding 4096 indices of `bits` each.
constexpr usize indexWords(u32 bits) noexcept {
    return kVolume * bits / 64;
}

// Palette bytes for an index width, rounded up to whole words so the block
// stays 8 byte aligned.
constexpr usize paletteBytes(u32 bits) noexcept {
    return ((usize{1} << bits) * sizeof(BlockStateId) + 7) & ~usize{7};
}

// The smallest supported index width for `entries` palette entries.
constexpr u32 bitsFor(usize entries) noexcept {
    if (entries <= 2) {
        return 1;
    }
    if (entries <= 4) {
        return 2;
    }
    return entries <= 16 ? 4 : 8;
}

constexpr u64 maskFor(u32 bits) noexcept {
    return (u64{1} << bits) - 1;
}

// Writes `value` as the index of block `index` into packed words.
void writePacked(u64* words, u32 bits, usize index, u64 value) noexcept {
    const usize bit = index * bits;
    const usize word = bit >> 6U;
    const u32 shift = static_cast<u32>(bit & 63U);
    words[word] = (words[word] & ~(maskFor(bits) << shift)) | (value << shift);
}

} // namespace

usize ChunkStorage::dataBytesFor(Mode mode, u32 bits) noexcept {
    switch (mode) {
    case Mode::Palette:
        return indexWords(bits) * sizeof(u64) + paletteBytes(bits);
    case Mode::Raw:
        return kRawBytes;
    case Mode::Uniform:
        break;
    }
    return 0;
}

u64* ChunkStorage::allocateData(usize bytes) {
    return static_cast<u64*>(memory::allocate(bytes, alignof(u64), MemoryTag::Chunks));
}

void ChunkStorage::freeData(u64* data, usize bytes) noexcept {
    memory::deallocate(data, bytes, alignof(u64), MemoryTag::Chunks);
}

ChunkStorage::~ChunkStorage() {
    release();
}

ChunkStorage::ChunkStorage(const ChunkStorage& other)
    : m_mode(other.m_mode)
    , m_bits(other.m_bits)
    , m_uniformState(other.m_uniformState)
    , m_paletteSize(other.m_paletteSize) {
    if (other.m_data != nullptr) {
        const usize bytes = other.dataBytes();
        m_data = allocateData(bytes);
        std::memcpy(m_data, other.m_data, bytes);
    }
}

ChunkStorage& ChunkStorage::operator=(const ChunkStorage& other) {
    if (this != &other) {
        ChunkStorage copy(other);
        *this = std::move(copy);
    }
    return *this;
}

ChunkStorage::ChunkStorage(ChunkStorage&& other) noexcept
    : m_data(std::exchange(other.m_data, nullptr))
    , m_mode(std::exchange(other.m_mode, Mode::Uniform))
    , m_bits(std::exchange(other.m_bits, u8{0}))
    , m_uniformState(std::exchange(other.m_uniformState, kAirState))
    , m_paletteSize(std::exchange(other.m_paletteSize, u16{0})) {}

ChunkStorage& ChunkStorage::operator=(ChunkStorage&& other) noexcept {
    if (this != &other) {
        release();
        m_data = std::exchange(other.m_data, nullptr);
        m_mode = std::exchange(other.m_mode, Mode::Uniform);
        m_bits = std::exchange(other.m_bits, u8{0});
        m_uniformState = std::exchange(other.m_uniformState, kAirState);
        m_paletteSize = std::exchange(other.m_paletteSize, u16{0});
    }
    return *this;
}

void ChunkStorage::release() noexcept {
    if (m_data != nullptr) {
        freeData(m_data, dataBytes());
        m_data = nullptr;
    }
    m_mode = Mode::Uniform;
    m_bits = 0;
    m_paletteSize = 0;
}

BlockStateId* ChunkStorage::palette() const noexcept {
    return reinterpret_cast<BlockStateId*>(m_data + indexWords(m_bits));
}

BlockStateId* ChunkStorage::raw() const noexcept {
    return reinterpret_cast<BlockStateId*>(m_data);
}

u32 ChunkStorage::paletteIndexAt(u16 index) const noexcept {
    const usize bit = usize{index} * m_bits;
    return static_cast<u32>((m_data[bit >> 6U] >> (bit & 63U)) & maskFor(m_bits));
}

void ChunkStorage::writePaletteIndex(u16 index, u32 paletteIndex) noexcept {
    writePacked(m_data, m_bits, index, paletteIndex);
}

BlockStateId ChunkStorage::getIndex(u16 index) const noexcept {
    VXE_ASSERT(index < kVolume, "Block index out of range");
    switch (m_mode) {
    case Mode::Palette:
        return palette()[paletteIndexAt(index)];
    case Mode::Raw:
        return raw()[index];
    case Mode::Uniform:
        break;
    }
    return m_uniformState;
}

void ChunkStorage::setIndex(u16 index, BlockStateId state) {
    VXE_ASSERT(index < kVolume, "Block index out of range");
    if (m_mode == Mode::Uniform) {
        if (state != m_uniformState) {
            startPalette(index, state);
        }
        return;
    }
    if (m_mode == Mode::Raw) {
        raw()[index] = state;
        return;
    }
    // Palette: reuse the state's entry if it has one (at most 256 to search,
    // usually a handful), otherwise add one, widening the indices if full.
    const BlockStateId* entries = palette();
    const BlockStateId* end = entries + m_paletteSize;
    const BlockStateId* found = std::find(entries, end, state);
    if (found != end) {
        writePaletteIndex(index, static_cast<u32>(found - entries));
        return;
    }
    if (m_paletteSize == (1U << m_bits)) {
        if (m_bits == 8) {
            switchToRaw();
            raw()[index] = state;
            return;
        }
        widen();
    }
    palette()[m_paletteSize] = state;
    writePaletteIndex(index, m_paletteSize);
    ++m_paletteSize;
}

void ChunkStorage::startPalette(u16 index, BlockStateId state) {
    // Uniform to a 1 bit palette: every index 0 (the old state), then the new
    // state as entry 1 at `index`.
    const usize bytes = dataBytesFor(Mode::Palette, 1);
    m_data = allocateData(bytes);
    std::memset(m_data, 0, bytes);
    m_mode = Mode::Palette;
    m_bits = 1;
    palette()[0] = m_uniformState;
    palette()[1] = state;
    m_paletteSize = 2;
    writePaletteIndex(index, 1);
}

void ChunkStorage::widen() {
    std::array<u16, 256> identity{};
    for (u16 i = 0; i < m_paletteSize; ++i) {
        identity.at(i) = i;
    }
    const std::vector<BlockStateId> states(palette(), palette() + m_paletteSize);
    repack(m_bits * 2U, identity, m_paletteSize, states);
}

void ChunkStorage::repack(u32 newBits, std::span<const u16> remap, u32 newPaletteSize,
                          std::span<const BlockStateId> states) {
    const usize newBytes = dataBytesFor(Mode::Palette, newBits);
    u64* words = allocateData(newBytes);
    std::memset(words, 0, newBytes);
    for (u16 i = 0; i < kVolume; ++i) {
        writePacked(words, newBits, i, remap[paletteIndexAt(i)]);
    }
    auto* newPalette = reinterpret_cast<BlockStateId*>(words + indexWords(newBits));
    std::ranges::copy(states, newPalette);
    freeData(m_data, dataBytes());
    m_data = words;
    m_bits = static_cast<u8>(newBits);
    m_paletteSize = static_cast<u16>(newPaletteSize);
}

void ChunkStorage::switchToRaw() {
    u64* words = allocateData(kRawBytes);
    auto* states = reinterpret_cast<BlockStateId*>(words);
    for (u16 i = 0; i < kVolume; ++i) {
        states[i] = palette()[paletteIndexAt(i)];
    }
    freeData(m_data, dataBytes());
    m_data = words;
    m_mode = Mode::Raw;
    m_bits = 0;
    m_paletteSize = 0;
}

void ChunkStorage::fill(BlockStateId state) noexcept {
    release();
    m_uniformState = state;
}

void ChunkStorage::assign(std::span<const BlockStateId, kChunkVolume> states) {
    // One pass builds the palette and each block's palette index. Generated
    // terrain comes in long runs of one state, so remembering the last
    // state's entry skips the palette search for most blocks.
    std::array<BlockStateId, 256> entries{};
    std::array<u8, kVolume> paletteIndices{};
    usize entryCount = 0;
    BlockStateId lastState = states[0];
    u8 lastIndex = 0;
    entries[0] = lastState;
    entryCount = 1;
    for (usize i = 0; i < kVolume; ++i) {
        const BlockStateId state = states[i];
        if (state != lastState) {
            const BlockStateId* begin = entries.data();
            const BlockStateId* end = begin + entryCount;
            const BlockStateId* found = std::find(begin, end, state);
            if (found == end) {
                if (entryCount == entries.size()) {
                    // More than 256 states: only raw can hold them.
                    release();
                    m_data = allocateData(kRawBytes);
                    std::memcpy(m_data, states.data(), kRawBytes);
                    m_mode = Mode::Raw;
                    return;
                }
                entries.at(entryCount) = state;
                ++entryCount;
            }
            lastState = state;
            lastIndex = static_cast<u8>(found - begin);
        }
        paletteIndices.at(i) = lastIndex;
    }
    if (entryCount == 1) {
        fill(entries[0]);
        return;
    }
    const u32 bits = bitsFor(entryCount);
    const usize bytes = dataBytesFor(Mode::Palette, bits);
    u64* words = allocateData(bytes);
    // Whole words at a time, the mirror image of decompressTo.
    const usize perWord = 64 / bits;
    usize block = 0;
    for (usize w = 0; w < indexWords(bits); ++w) {
        u64 word = 0;
        for (usize k = 0; k < perWord; ++k) {
            word |= u64{paletteIndices.at(block++)} << (k * bits);
        }
        words[w] = word;
    }
    std::memset(words + indexWords(bits), 0, paletteBytes(bits));
    std::copy(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(entryCount),
              reinterpret_cast<BlockStateId*>(words + indexWords(bits)));
    release();
    m_data = words;
    m_mode = Mode::Palette;
    m_bits = static_cast<u8>(bits);
    m_paletteSize = static_cast<u16>(entryCount);
}

void ChunkStorage::decompressTo(std::span<BlockStateId, kChunkVolume> out) const noexcept {
    switch (m_mode) {
    case Mode::Uniform:
        std::ranges::fill(out, m_uniformState);
        return;
    case Mode::Raw:
        std::memcpy(out.data(), raw(), kRawBytes);
        return;
    case Mode::Palette:
        break;
    }
    // Whole words at a time: each word holds 64 / bits indices in order.
    const BlockStateId* entries = palette();
    const u32 bits = m_bits;
    const u64 mask = maskFor(bits);
    const usize perWord = 64 / bits;
    usize block = 0;
    for (usize w = 0; w < indexWords(bits); ++w) {
        u64 word = m_data[w];
        for (usize k = 0; k < perWord; ++k) {
            out[block++] = entries[word & mask];
            word >>= bits;
        }
    }
}

void ChunkStorage::compact() {
    if (m_mode == Mode::Uniform) {
        return;
    }
    if (m_mode == Mode::Palette) {
        std::array<bool, 256> used{};
        for (u16 i = 0; i < kVolume; ++i) {
            used.at(paletteIndexAt(i)) = true;
        }
        std::array<u16, 256> remap{};
        std::vector<BlockStateId> states;
        for (u16 i = 0; i < m_paletteSize; ++i) {
            if (used.at(i)) {
                remap.at(i) = static_cast<u16>(states.size());
                states.push_back(palette()[i]);
            }
        }
        if (states.size() == 1) {
            fill(states.front());
            return;
        }
        const u32 bits = bitsFor(states.size());
        if (bits != m_bits || states.size() != m_paletteSize) {
            repack(bits, remap, static_cast<u32>(states.size()), states);
        }
        return;
    }
    // Raw: back to a palette once 256 or fewer states are left.
    std::vector<BlockStateId> states(raw(), raw() + kVolume);
    std::ranges::sort(states);
    const auto duplicates = std::ranges::unique(states);
    states.erase(duplicates.begin(), duplicates.end());
    if (states.size() == 1) {
        fill(states.front());
        return;
    }
    if (states.size() > 256) {
        return;
    }
    const u32 bits = bitsFor(states.size());
    const usize bytes = dataBytesFor(Mode::Palette, bits);
    u64* words = allocateData(bytes);
    std::memset(words, 0, bytes);
    for (u16 i = 0; i < kVolume; ++i) {
        const auto entry = std::ranges::lower_bound(states, raw()[i]) - states.begin();
        writePacked(words, bits, i, static_cast<u64>(entry));
    }
    std::ranges::copy(states, reinterpret_cast<BlockStateId*>(words + indexWords(bits)));
    freeData(m_data, kRawBytes);
    m_data = words;
    m_mode = Mode::Palette;
    m_bits = static_cast<u8>(bits);
    m_paletteSize = static_cast<u16>(states.size());
}

} // namespace vxe::voxel
