#include "vxe/voxel/lod_downsample.hpp"

#include <vxe/core/profile.hpp>

#include <algorithm>
#include <limits>

namespace vxe::voxel {
namespace {

// Sorts spans from the top down and joins those that overlap or touch, so
// the result passes validLodSpans (apart from the count). The joined span
// keeps the materials of the one reaching highest.
void joinSpans(std::vector<LodSpan>& spans) {
    std::ranges::sort(spans, [](const LodSpan& a, const LodSpan& b) { return a.topY > b.topY; });
    usize kept = 0;
    for (usize i = 0; i < spans.size(); ++i) {
        if (kept > 0 && i64{spans[i].topY} + 1 >= spans[kept - 1].bottomY) {
            spans[kept - 1].bottomY = std::min(spans[kept - 1].bottomY, spans[i].bottomY);
            continue;
        }
        spans[kept++] = spans[i];
    }
    spans.resize(kept);
}

} // namespace

void limitLodSpans(std::vector<LodSpan>& spans, usize maxSpans) {
    maxSpans = std::max<usize>(maxSpans, 1);
    while (spans.size() > maxSpans) {
        usize joinAt = 0;
        i64 smallest = std::numeric_limits<i64>::max();
        for (usize i = 0; i + 1 < spans.size(); ++i) {
            const i64 gap = i64{spans[i].bottomY} - spans[i + 1].topY;
            if (gap < smallest) {
                smallest = gap;
                joinAt = i;
            }
        }
        spans[joinAt].bottomY = spans[joinAt + 1].bottomY;
        spans.erase(spans.begin() + static_cast<std::ptrdiff_t>(joinAt) + 1);
    }
}

void downsampleChunkColumn(std::span<const ChunkStorage* const> chunks, i32 firstChunkY,
                           const BlockRegistryView& blocks, usize maxSpans,
                           std::array<std::vector<LodSpan>, kLodCellsPerColumn>& out) {
    VXE_PROFILE_FUNCTION();
    // The column's states, decompressed once: chunk i at states[i].
    std::vector<std::array<BlockStateId, kChunkVolume>> states(chunks.size());
    for (usize i = 0; i < chunks.size(); ++i) {
        if (chunks[i] != nullptr) {
            chunks[i]->decompressTo(states[i]);
        } else {
            states[i].fill(kAirState);
        }
    }
    const auto height = static_cast<i32>(chunks.size()) * kChunkSize;
    const i64 baseY = i64{firstChunkY} * kChunkSize;
    for (i32 cz = 0; cz < kLodCellsPerChunk; ++cz) {
        for (i32 cx = 0; cx < kLodCellsPerChunk; ++cx) {
            std::vector<LodSpan>& spans =
                out.at((static_cast<usize>(cz) * kLodCellsPerChunk) + static_cast<usize>(cx));
            spans.clear();
            bool inRun = false;
            for (i32 y = height - 1; y >= 0; --y) {
                const auto& layer = states[static_cast<usize>(y / kChunkSize)];
                const i32 localY = y % kChunkSize;
                // The first visible block among the cell's 2 x 2 columns.
                const BlockRenderInfo* visible = nullptr;
                for (i32 i = 0; i < 4 && visible == nullptr; ++i) {
                    const i32 x = (cx * 2) + (i & 1);
                    const i32 z = (cz * 2) + (i >> 1);
                    const BlockStateId state = layer[(static_cast<usize>(localY) * 256) +
                                                     (static_cast<usize>(z) * 16) + static_cast<usize>(x)];
                    const BlockRenderInfo& info = blocks.info(state);
                    if (info.layer != RenderLayer::Invisible) {
                        visible = &info;
                    }
                }
                const auto worldY = static_cast<i32>(baseY + y);
                if (visible != nullptr && !inRun) {
                    spans.push_back({.bottomY = worldY,
                                     .topY = worldY,
                                     .topMaterial = visible->textureLayer(BlockFace::PosY),
                                     .sideMaterial = visible->textureLayer(BlockFace::PosX)});
                    inRun = true;
                } else if (visible != nullptr) {
                    spans.back().bottomY = worldY;
                } else {
                    inRun = false;
                }
            }
            limitLodSpans(spans, maxSpans);
        }
    }
}

void replaceLodSpanRange(std::span<const LodSpan> old, std::span<const LodSpan> inside, i64 low, i64 high,
                         usize maxSpans, std::vector<LodSpan>& out) {
    out.clear();
    for (const LodSpan& span : old) {
        if (span.topY > high) {
            LodSpan above = span;
            above.bottomY = static_cast<i32>(std::max<i64>(span.bottomY, high + 1));
            out.push_back(above);
        }
        if (span.bottomY < low) {
            LodSpan below = span;
            below.topY = static_cast<i32>(std::min<i64>(span.topY, low - 1));
            out.push_back(below);
        }
    }
    for (const LodSpan& span : inside) {
        out.push_back(span);
    }
    joinSpans(out);
    limitLodSpans(out, maxSpans);
}

void downsampleLodCells(std::array<std::span<const LodSpan>, 4> children, usize maxSpans,
                        std::vector<LodSpan>& out) {
    out.clear();
    for (const std::span<const LodSpan> child : children) {
        out.insert(out.end(), child.begin(), child.end());
    }
    joinSpans(out);
    limitLodSpans(out, maxSpans);
}

void downsampleLodTile(const LodTile& child, LodTile& parent, usize maxSpans) {
    VXE_PROFILE_FUNCTION();
    const auto offsetX = static_cast<u32>(child.key().x & 1) * (kLodTileCells / 2);
    const auto offsetZ = static_cast<u32>(child.key().z & 1) * (kLodTileCells / 2);
    // Rebuilt whole with a builder: replacing 1024 cells one by one would
    // move the spans after each of them every time.
    LodTileBuilder builder(parent.key());
    std::vector<LodSpan> cell;
    for (u32 pz = 0; pz < kLodTileCells; ++pz) {
        for (u32 px = 0; px < kLodTileCells; ++px) {
            const bool inside = px >= offsetX && px < offsetX + (kLodTileCells / 2) && pz >= offsetZ &&
                                pz < offsetZ + (kLodTileCells / 2);
            if (!inside) {
                builder.addCell(parent.cell(px, pz));
                continue;
            }
            const u32 cx = (px - offsetX) * 2;
            const u32 cz = (pz - offsetZ) * 2;
            downsampleLodCells({child.cell(cx, cz), child.cell(cx + 1, cz), child.cell(cx, cz + 1),
                                child.cell(cx + 1, cz + 1)},
                               maxSpans, cell);
            builder.addCell(cell);
        }
    }
    parent = std::move(builder).build();
}

} // namespace vxe::voxel
