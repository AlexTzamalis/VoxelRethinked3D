#pragma once

#include <vxe/core/hash.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace vxe::voxel {

// A column of chunks: every chunk with this x and z, at any height.
struct ColumnPos {
    i32 x = 0;
    i32 z = 0;

    friend constexpr auto operator<=>(const ColumnPos&, const ColumnPos&) = default;
};

[[nodiscard]] constexpr ColumnPos columnOf(const ChunkPos& chunk) noexcept {
    return {chunk.x, chunk.z};
}

// What is known about one column of chunks without looking at every chunk
// in it (Section 9.5, glossary): per block column (16 x 16 per chunk
// column), the highest loaded non air block, and a coarse surface estimate
// from the generator for heights that are not loaded. Lighting uses it to
// find where sky light starts (E10.3), streaming to rank chunks near the
// surface first, LOD to place far terrain.
//
// "Non air" means any state other than kAirState: block properties such as
// light opacity live in the block registry view, which the world does not
// hold. Sky light (E10.3) decides whether it needs an opacity based height
// on top of this one.
//
// Heights are block Y values as i32 (plus or minus 2 billion blocks, far
// beyond any world height of Section 9.2); kNoBlock means nothing is known.
//
// Kept up to date by VoxelWorld on every chunk insert, removal and block
// change, so reading it is free. Thread safety: like the world, owned by
// one thread.
class ColumnSummary {
public:
    static constexpr i32 kNoBlock = std::numeric_limits<i32>::min();

    explicit ColumnSummary(ColumnPos position) noexcept
        : m_position(position) {
        m_highest.fill(kNoBlock);
    }

    [[nodiscard]] ColumnPos position() const noexcept { return m_position; }

    // The highest loaded non air block of block column (x, z), both 0 to 15.
    [[nodiscard]] i32 highestBlock(u32 x, u32 z) const noexcept { return m_highest[(z * 16) + x]; }
    // The highest loaded non air block of the whole chunk column.
    [[nodiscard]] i32 highestBlock() const noexcept { return std::ranges::max(m_highest); }

    // A surface height from the world generator (the game's chunk source), so
    // streaming and lighting can reason about chunks that are not loaded.
    [[nodiscard]] i32 surfaceEstimate() const noexcept { return m_surfaceEstimate; }

    // The Y of every loaded chunk in this column, lowest first.
    [[nodiscard]] std::span<const i32> loadedChunkYs() const noexcept { return m_chunkYs; }

private:
    friend class VoxelWorld;

    ColumnPos m_position;
    i32 m_surfaceEstimate = kNoBlock;
    std::array<i32, kChunkArea> m_highest{}; // Index z * 16 + x.
    std::vector<i32> m_chunkYs;
};

} // namespace vxe::voxel

template <>
struct std::hash<vxe::voxel::ColumnPos> {
    [[nodiscard]] std::size_t operator()(const vxe::voxel::ColumnPos& p) const noexcept {
        const vxe::u64 h = vxe::hashCombine(0, static_cast<vxe::u32>(p.x));
        return static_cast<std::size_t>(vxe::hashCombine(h, static_cast<vxe::u32>(p.z)));
    }
};
