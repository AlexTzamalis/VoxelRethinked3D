#include "vxe/voxel/block_light.hpp"

#include <vxe/core/profile.hpp>

#include <algorithm>

namespace vxe::voxel {
namespace {

constexpr i32 kSize = 3 * kChunkSize; // The neighborhood box: 48 blocks per side.
constexpr i32 kStrideY = kSize * kSize;
constexpr i32 kStrideZ = kSize;

// The box cell of a block of neighborhood chunk k (neighborBit order).
constexpr u32 cellOf(usize k, i32 x, i32 y, i32 z) noexcept {
    const i32 baseX = static_cast<i32>(k % 3) * kChunkSize;
    const i32 baseY = static_cast<i32>(k / 9) * kChunkSize;
    const i32 baseZ = static_cast<i32>((k / 3) % 3) * kChunkSize;
    return static_cast<u32>(((baseY + y) * kStrideY) + ((baseZ + z) * kStrideZ) + baseX + x);
}

} // namespace

bool neighborhoodEmitsLight(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks) {
    return std::ranges::any_of(chunks, [&blocks](const ChunkStorage* chunk) {
        return chunk != nullptr &&
               chunk->anyState([&blocks](BlockStateId state) { return blocks.info(state).emittedLight > 0; });
    });
}

BlockLighter::BlockLighter()
    : m_attenuation(kCells)
    , m_level(kCells) {}

void BlockLighter::compute(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks,
                           std::span<u8, kChunkVolume> out) {
    VXE_PROFILE_FUNCTION();
    for (auto& bucket : m_queue) {
        bucket.clear();
    }
    std::ranges::fill(m_level, u8{0});
    for (usize k = 0; k < chunks.size(); ++k) {
        fillChunk(k, chunks.at(k), blocks);
    }
    spread();
    for (i32 y = 0; y < kChunkSize; ++y) {
        for (i32 z = 0; z < kChunkSize; ++z) {
            const u32 row = cellOf(13, 0, y, z);
            const usize first = (static_cast<usize>(y) * 256) + (static_cast<usize>(z) * 16);
            std::copy_n(m_level.begin() + row, kChunkSize, out.subspan(first, kChunkSize).begin());
        }
    }
}

// Puts one chunk of the neighborhood into the box: what each cell costs
// light to enter, and the light sources with their level.
void BlockLighter::fillChunk(usize k, const ChunkStorage* chunk, const BlockRegistryView& blocks) {
    if (chunk == nullptr || chunk->isUniform()) {
        const BlockRenderInfo& info = chunk == nullptr ? airRenderInfo() : blocks.info(chunk->uniformState());
        const u8 cost = std::max<u8>(info.lightOpacity, 1);
        for (i32 y = 0; y < kChunkSize; ++y) {
            for (i32 z = 0; z < kChunkSize; ++z) {
                std::fill_n(m_attenuation.begin() + cellOf(k, 0, y, z), kChunkSize, cost);
            }
        }
        if (info.emittedLight == 0) {
            return;
        }
        for (u16 i = 0; i < kChunkVolume; ++i) {
            const LocalPos local = LocalPos::fromIndex(i);
            addSource(cellOf(k, local.x, local.y, local.z), info.emittedLight);
        }
        return;
    }
    chunk->decompressTo(m_states);
    for (u16 i = 0; i < kChunkVolume; ++i) {
        const LocalPos local = LocalPos::fromIndex(i);
        const BlockRenderInfo& info = blocks.info(m_states.at(i));
        const u32 cell = cellOf(k, local.x, local.y, local.z);
        m_attenuation[cell] = std::max<u8>(info.lightOpacity, 1);
        if (info.emittedLight > 0) {
            addSource(cell, info.emittedLight);
        }
    }
}

void BlockLighter::addSource(u32 cell, u8 level) {
    m_level[cell] = level;
    m_queue.at(level).push_back(cell);
}

// Spreads from the brightest cells down. A cell queued at a level it no
// longer has was reached brighter later; it is skipped.
void BlockLighter::spread() {
    for (u32 level = kMaxLightLevel; level > 1; --level) {
        std::vector<u32>& bucket = m_queue.at(level);
        // An index, not an iterator: lower buckets grow while this one is
        // walked, and this one never does (light only goes down).
        for (usize head = 0; head < bucket.size(); ++head) { // NOLINT(modernize-loop-convert)
            const u32 cell = bucket[head];
            if (m_level[cell] != level) {
                continue;
            }
            const auto x = static_cast<i32>(cell % kSize);
            const auto z = static_cast<i32>((cell / kSize) % kSize);
            const auto y = static_cast<i32>(cell / kStrideY);
            const auto reach = [&](bool inside, i32 step) {
                if (!inside) {
                    return;
                }
                const auto next = static_cast<u32>(static_cast<i32>(cell) + step);
                const u8 cost = m_attenuation[next];
                if (cost >= kMaxLightLevel || cost >= level) {
                    return;
                }
                const auto reached = static_cast<u8>(level - cost);
                if (reached > m_level[next]) {
                    m_level[next] = reached;
                    m_queue.at(reached).push_back(next);
                }
            };
            reach(x + 1 < kSize, 1);
            reach(x > 0, -1);
            reach(y + 1 < kSize, kStrideY);
            reach(y > 0, -kStrideY);
            reach(z + 1 < kSize, kStrideZ);
            reach(z > 0, -kStrideZ);
        }
    }
}

} // namespace vxe::voxel
