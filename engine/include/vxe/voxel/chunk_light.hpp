#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>

#include <span>

namespace vxe::voxel {

// Light levels run from 0 (dark) to 15 (full), for sky light and block light
// alike (Section 14). One block's light is packed into a byte: sky light in
// the high 4 bits, block light in the low 4.
inline constexpr u8 kMaxLight = 15;

[[nodiscard]] constexpr u8 packLight(u8 sky, u8 block) noexcept {
    return static_cast<u8>((static_cast<u32>(sky) << 4U) | (static_cast<u32>(block) & 0x0FU));
}
[[nodiscard]] constexpr u8 skyLightOf(u8 packed) noexcept {
    return static_cast<u8>(packed >> 4U);
}
[[nodiscard]] constexpr u8 blockLightOf(u8 packed) noexcept {
    return static_cast<u8>(packed & 0x0FU);
}

// Open sky, no block light: what chunks hold until the light engine (E10.2,
// E10.3) computes them, which matches the mesher's placeholder light.
inline constexpr u8 kOpenSkyLight = packLight(kMaxLight, 0);
// Fully dark: deep underground.
inline constexpr u8 kDarkLight = packLight(0, 0);

// The sky and block light of the 4096 blocks of one chunk (Section 14.2), in
// the same block order as ChunkStorage (y * 256 + z * 16 + x).
//
// Most chunks are lit the same everywhere: open sky above the terrain, dark
// deep below. Those are Uniform and cost no heap memory (the value lives in
// the object). Any other chunk is Full: one byte per block, 4096 bytes. A
// write that makes a uniform chunk differ switches it to Full on its own;
// compact() switches back when every block is lit the same again. Light jobs
// produce whole chunks and call assign(), which picks the mode itself.
//
// Full storage is allocated under MemoryTag::Chunks.
//
// Thread safety: a plain value. The world's owner thread changes it; jobs
// read copies in snapshots.
class ChunkLight {
public:
    ChunkLight() noexcept = default; // Uniform open sky.
    explicit ChunkLight(u8 packed) noexcept
        : m_uniform(packed) {}
    ~ChunkLight();
    ChunkLight(const ChunkLight& other);
    ChunkLight& operator=(const ChunkLight& other);
    ChunkLight(ChunkLight&& other) noexcept;
    ChunkLight& operator=(ChunkLight&& other) noexcept;

    // Packed light (packLight) of one block.
    [[nodiscard]] u8 get(LocalPos position) const noexcept { return getIndex(position.index()); }
    [[nodiscard]] u8 getIndex(u16 index) const noexcept {
        return m_data != nullptr ? m_data[index] : m_uniform;
    }
    [[nodiscard]] u8 sky(LocalPos position) const noexcept { return skyLightOf(get(position)); }
    [[nodiscard]] u8 block(LocalPos position) const noexcept { return blockLightOf(get(position)); }

    void set(LocalPos position, u8 packed) { setIndex(position.index(), packed); }
    void setIndex(u16 index, u8 packed);
    // Every block the same, freeing full storage.
    void fill(u8 packed) noexcept;
    // All 4096 values at once; uniform if they are all the same.
    void assign(std::span<const u8, kChunkVolume> packed);
    // Switches back to Uniform if every block has the same light. Returns
    // true if it did. A scan of 4096 bytes, so for jobs, not per edit.
    bool compact() noexcept;

    // Copies the light into a box with its own row and slice strides, like
    // ChunkStorage::decompressTo; the snapshot builder uses it.
    void copyTo(std::span<u8> out, usize rowStride, usize sliceStride) const noexcept;

    [[nodiscard]] bool isUniform() const noexcept { return m_data == nullptr; }
    // The value of a uniform chunk (meaningless for a full one).
    [[nodiscard]] u8 uniformValue() const noexcept { return m_uniform; }
    [[nodiscard]] usize memoryBytes() const noexcept {
        return sizeof(ChunkLight) + (m_data != nullptr ? usize{kChunkVolume} : 0);
    }

    // Equal light in every block, whatever the modes.
    [[nodiscard]] friend bool operator==(const ChunkLight& a, const ChunkLight& b) noexcept {
        return a.equals(b);
    }

private:
    [[nodiscard]] bool equals(const ChunkLight& other) const noexcept;
    void release() noexcept;

    u8* m_data = nullptr; // Full mode: 4096 values. Null in Uniform mode.
    u8 m_uniform = kOpenSkyLight;
};

} // namespace vxe::voxel
