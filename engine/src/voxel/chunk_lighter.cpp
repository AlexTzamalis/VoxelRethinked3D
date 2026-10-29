#include "vxe/voxel/chunk_lighter.hpp"

#include <vxe/core/profile.hpp>
#include <vxe/voxel/chunk_light.hpp>

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

// Steps along one axis from a box coordinate to the middle chunk (16 to 31).
constexpr i32 stepsToMiddle(i32 value) noexcept {
    if (value < kChunkSize) {
        return kChunkSize - value;
    }
    return value >= 2 * kChunkSize ? value - (2 * kChunkSize) + 1 : 0;
}

} // namespace

bool neighborhoodEmitsLight(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks) {
    return std::ranges::any_of(chunks, [&blocks](const ChunkStorage* chunk) {
        return chunk != nullptr &&
               chunk->anyState([&blocks](BlockStateId state) { return blocks.info(state).emittedLight > 0; });
    });
}

ChunkLighter::ChunkLighter()
    : m_opacity(kCells)
    , m_level(kCells) {}

void ChunkLighter::compute(const ChunkNeighborhood& chunks, const BlockRegistryView& blocks,
                           const SkyOpenings& sky, bool blockLight, std::span<u8, kChunkVolume> out) {
    VXE_PROFILE_FUNCTION();
    m_sources.clear();
    m_sourceLevels.clear();
    // Opacity in the low and light emitted in the high 4 bits, per state.
    m_lightOf.resize(blocks.size());
    for (usize state = 0; state < blocks.size(); ++state) {
        const BlockRenderInfo& info = blocks.info(static_cast<BlockStateId>(state));
        m_lightOf[state] = static_cast<u8>(std::min<u32>(info.lightOpacity, kMaxLightLevel) |
                                           (blockLight ? u32{info.emittedLight} << 4U : 0U));
    }
    for (usize k = 0; k < chunks.size(); ++k) {
        fillChunk(k, chunks.at(k), blocks, blockLight);
    }
    const auto copyMiddle = [this](std::span<u8, kChunkVolume> target) {
        for (i32 y = 0; y < kChunkSize; ++y) {
            for (i32 z = 0; z < kChunkSize; ++z) {
                const u32 row = cellOf(13, 0, y, z);
                const usize first = (static_cast<usize>(y) * 256) + (static_cast<usize>(z) * 16);
                std::copy_n(m_level.begin() + row, kChunkSize, target.subspan(first, kChunkSize).begin());
            }
        }
    };

    // Block light first, kept aside, then sky light in the same buffers.
    std::ranges::fill(m_level, u8{0});
    for (auto& bucket : m_queue) {
        bucket.clear();
    }
    for (usize i = 0; i < m_sources.size(); ++i) {
        addSource(m_sources[i], m_sourceLevels[i]);
    }
    spread();
    copyMiddle(m_blockLight);

    std::ranges::fill(m_level, u8{0});
    for (auto& bucket : m_queue) {
        bucket.clear();
    }
    seedSky(sky);
    spread();
    copyMiddle(out);
    for (usize i = 0; i < kChunkVolume; ++i) {
        out[i] = packLight(out[i], m_blockLight.at(i));
    }
}

// Puts one chunk of the neighborhood into the box: the light opacity of each
// cell, and the light sources.
void ChunkLighter::fillChunk(usize k, const ChunkStorage* chunk, const BlockRegistryView& blocks,
                             bool blockLight) {
    if (chunk == nullptr || chunk->isUniform()) {
        const BlockRenderInfo& info = chunk == nullptr ? airRenderInfo() : blocks.info(chunk->uniformState());
        for (i32 y = 0; y < kChunkSize; ++y) {
            for (i32 z = 0; z < kChunkSize; ++z) {
                std::fill_n(m_opacity.begin() + cellOf(k, 0, y, z), kChunkSize, info.lightOpacity);
            }
        }
        if (!blockLight || info.emittedLight == 0) {
            return;
        }
        for (u16 i = 0; i < kChunkVolume; ++i) {
            const LocalPos local = LocalPos::fromIndex(i);
            m_sources.push_back(cellOf(k, local.x, local.y, local.z));
            m_sourceLevels.push_back(info.emittedLight);
        }
        return;
    }
    chunk->decompressTo(m_states);
    // Row by row: the box rows of a chunk are 16 cells in a row, and the
    // states come in the same order, so this is a straight copy through the
    // registry (110,592 cells per neighborhood, so every cycle shows).
    const BlockStateId* state = m_states.data();
    for (i32 y = 0; y < kChunkSize; ++y) {
        for (i32 z = 0; z < kChunkSize; ++z) {
            const u32 row = cellOf(k, 0, y, z);
            u8* opacity = m_opacity.data() + row;
            for (i32 x = 0; x < kChunkSize; ++x, ++state) {
                // States the registry does not know read as air, like info().
                const u8 light = *state < m_lightOf.size() ? m_lightOf[*state] : u8{0};
                opacity[x] = static_cast<u8>(light & 0x0FU);
                if (light > 0x0FU) {
                    m_sources.push_back(row + static_cast<u32>(x));
                    m_sourceLevels.push_back(static_cast<u8>(light >> 4U));
                }
            }
        }
    }
}

void ChunkLighter::addSource(u32 cell, u8 level) {
    if (level > m_level[cell]) {
        m_level[cell] = level;
        m_queue.at(level).push_back(cell);
    }
}

// Full sky light down every open column, without loss through clear blocks,
// until something dims or stops it. Below that the flood takes over.
void ChunkLighter::seedSky(const SkyOpenings& sky) {
    for (i32 z = 0; z < kSize; ++z) {
        for (i32 x = 0; x < kSize; ++x) {
            if (!sky.test((static_cast<usize>(z) * kSize) + static_cast<usize>(x))) {
                continue;
            }
            for (i32 y = kSize - 1; y >= 0; --y) {
                const auto cell = static_cast<u32>((y * kStrideY) + (z * kStrideZ) + x);
                const u8 opacity = m_opacity[cell];
                if (opacity >= kMaxLightLevel) {
                    break;
                }
                addSource(cell, static_cast<u8>(kMaxLightLevel - opacity));
                if (opacity > 0) {
                    break; // Dimmed: the flood spreads it on, with loss.
                }
            }
        }
    }
}

// Spreads from the brightest cells down, at least one level per block. A
// cell queued at a level it no longer has was reached brighter later; it is
// skipped.
void ChunkLighter::spread() {
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
            // Every step costs at least one level, so light from here only
            // matters if it can still reach the middle chunk: skip cells at
            // least `level` steps away. Most of the box is that far.
            if (stepsToMiddle(x) + stepsToMiddle(y) + stepsToMiddle(z) >= static_cast<i32>(level)) {
                continue;
            }
            const auto reach = [&](bool inside, i32 step) {
                if (!inside) {
                    return;
                }
                const auto next = static_cast<u32>(static_cast<i32>(cell) + step);
                const u8 cost = std::max<u8>(m_opacity[next], 1);
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
