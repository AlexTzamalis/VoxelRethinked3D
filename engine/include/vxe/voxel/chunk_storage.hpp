#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <span>

namespace vxe::voxel {

// The 4096 block states of one 16x16x16 chunk in as few bytes as possible
// (Section 9.4). Cubic worlds are mostly all air or all stone, so those cost
// nothing beyond this 16 byte object; mixed chunks store a small palette of
// the states they use plus a bit packed index per block.
//
// Modes and their memory:
//   Uniform  every block is one state             16 bytes, no heap block
//   Palette  2 to 256 states, 1, 2, 4 or 8 bits   indices (512 to 4096 bytes) + palette
//   Raw      more than 256 states                 8192 bytes, one u16 per block
//
// Index widths are 1, 2, 4 or 8 bits so an index never crosses a 64 bit word:
// a read is one load, one shift and one mask. Writes switch modes on their
// own (the first stone in an all air chunk makes a 1 bit palette; a 3rd
// state widens to 2 bits). Overwritten states stay in the palette until
// compact() runs, which a background job does, so a write never has to scan
// the whole chunk.
//
// Block order is y * 256 + z * 16 + x (LocalPos::index), so a horizontal
// slice is contiguous.
//
// Memory: one heap block from the Chunks memory tag, holding the packed
// indices followed by the palette (or the raw states).
//
// Thread safety: none. A chunk's storage belongs to one thread (the server);
// jobs get decompressed copies (snapshots, Section 9.5).
class ChunkStorage {
public:
    enum class Mode : u8 {
        Uniform,
        Palette,
        Raw
    };

    ChunkStorage() noexcept = default; // All air.
    explicit ChunkStorage(BlockStateId state) noexcept
        : m_uniformState(state) {}
    ~ChunkStorage();
    ChunkStorage(const ChunkStorage& other);
    ChunkStorage& operator=(const ChunkStorage& other);
    ChunkStorage(ChunkStorage&& other) noexcept;
    ChunkStorage& operator=(ChunkStorage&& other) noexcept;

    [[nodiscard]] BlockStateId get(LocalPos position) const noexcept { return getIndex(position.index()); }
    [[nodiscard]] BlockStateId getIndex(u16 index) const noexcept;
    void set(LocalPos position, BlockStateId state) { setIndex(position.index(), state); }
    void setIndex(u16 index, BlockStateId state);

    // Every block one state, freeing the heap block. World generation uses
    // it for the common all air and all stone chunks.
    void fill(BlockStateId state) noexcept;

    // Replaces all 4096 states (in block order) and picks the smallest mode
    // that holds them, as compact() would. For generated or loaded chunks.
    void assign(std::span<const BlockStateId, kChunkVolume> states);

    // True if any state the chunk holds passes `test`, without unpacking:
    // the one state of a uniform chunk, the palette of a palette chunk (which
    // may still list states overwritten since the last compact(), so the
    // answer can be a false yes, never a false no), or every block of a raw
    // chunk. The light engine asks it whether a chunk emits light.
    template <typename Test>
    [[nodiscard]] bool anyState(Test&& test) const {
        if (m_mode == Mode::Uniform) {
            return test(m_uniformState);
        }
        const std::span<const BlockStateId> states(m_mode == Mode::Palette ? palette() : raw(),
                                                   m_mode == Mode::Palette ? m_paletteSize : kChunkVolume);
        for (const BlockStateId state : states) {
            if (test(state)) {
                return true;
            }
        }
        return false;
    }

    // All 4096 states into a flat array in block order, so meshing and
    // lighting loops never touch the packed format.
    void decompressTo(std::span<BlockStateId, kChunkVolume> out) const noexcept;

    // All 4096 states into a larger 3D box: block (x, y, z) goes to
    // out[y * sliceStride + z * rowStride + x]. Snapshots use it to write a
    // chunk straight into the inside of their padded 18^3 box, which saves
    // decompressing into a flat array first and copying 256 rows after.
    // `out` must hold 15 * sliceStride + 15 * rowStride + 16 states.
    void decompressTo(std::span<BlockStateId> out, usize rowStride, usize sliceStride) const noexcept;

    // Drops palette entries no block uses any more and picks the smallest
    // mode and index width that fits: back to Uniform when one state is
    // left, from Raw to Palette at 256 states or fewer. Scans the chunk, so
    // it belongs on a background job, not after every write.
    void compact();

    [[nodiscard]] Mode mode() const noexcept { return m_mode; }
    [[nodiscard]] bool isUniform() const noexcept { return m_mode == Mode::Uniform; }
    // The state of a Uniform chunk.
    [[nodiscard]] BlockStateId uniformState() const noexcept { return m_uniformState; }
    [[nodiscard]] u32 bitsPerIndex() const noexcept { return m_bits; }
    // Palette entries, including unused ones until compact().
    [[nodiscard]] u32 paletteSize() const noexcept { return m_paletteSize; }
    // Heap bytes of block data (0 for Uniform).
    [[nodiscard]] usize dataBytes() const noexcept { return dataBytesFor(m_mode, m_bits); }
    // Everything this chunk's blocks cost: the object plus its heap block.
    [[nodiscard]] usize memoryBytes() const noexcept { return sizeof(ChunkStorage) + dataBytes(); }

private:
    [[nodiscard]] static usize dataBytesFor(Mode mode, u32 bits) noexcept;
    [[nodiscard]] static u64* allocateData(usize bytes);
    static void freeData(u64* data, usize bytes) noexcept;
    [[nodiscard]] BlockStateId* palette() const noexcept;
    [[nodiscard]] BlockStateId* raw() const noexcept;
    [[nodiscard]] u32 paletteIndexAt(u16 index) const noexcept;
    void writePaletteIndex(u16 index, u32 paletteIndex) noexcept;
    void startPalette(u16 index, BlockStateId state);
    void widen();
    void switchToRaw();
    void repack(u32 newBits, std::span<const u16> remap, u32 newPaletteSize,
                std::span<const BlockStateId> states);
    void release() noexcept;

    u64* m_data = nullptr; // Packed indices, then the palette; or the raw states.
    Mode m_mode = Mode::Uniform;
    u8 m_bits = 0; // 1, 2, 4 or 8 in Palette mode.
    BlockStateId m_uniformState = kAirState;
    u16 m_paletteSize = 0;
};

} // namespace vxe::voxel
