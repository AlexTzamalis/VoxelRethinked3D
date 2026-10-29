#include "vxe/voxel/lod_mesher.hpp"

#include <vxe/core/profile.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <tuple>
#include <utility>

namespace vxe::voxel {
namespace {

// Face numbers, as in BlockFace and the chunk quads.
constexpr u8 kPosX = 0;
constexpr u8 kNegX = 1;
constexpr u8 kPosY = 2;
constexpr u8 kNegY = 3;
constexpr u8 kPosZ = 4;
constexpr u8 kNegZ = 5;
constexpr i32 kCells = kLodTileCells;

[[nodiscard]] u32 cellIndex(i32 cx, i32 cz) noexcept {
    return static_cast<u32>((cz * kCells) + cx);
}

// True if the highest spans of two cells give the same top face, so the tops
// can merge into one rectangle.
[[nodiscard]] bool sameTop(std::span<const LodSpan> a, std::span<const LodSpan> b) noexcept {
    return !a.empty() && !b.empty() && a.front().topY == b.front().topY &&
           a.front().topMaterial == b.front().topMaterial && a.front().skyLight == b.front().skyLight;
}

// Calls emit(low, high) for each run of blocks of `span` that no span of
// `neighbor` covers, from the top down. Spans go from the top down and do
// not touch, so one walk over the neighbor's spans is enough.
template <typename Emit>
void forEachOpenInterval(const LodSpan& span, std::span<const LodSpan> neighbor, const Emit& emit) {
    i64 high = span.topY;
    for (const LodSpan& other : neighbor) {
        if (other.bottomY > high) {
            continue;
        }
        if (other.topY < span.bottomY) {
            break;
        }
        if (other.topY < high) {
            emit(std::max<i64>(i64{other.topY} + 1, span.bottomY), high);
        }
        high = i64{other.bottomY} - 1;
        if (high < span.bottomY) {
            return;
        }
    }
    if (high >= span.bottomY) {
        emit(span.bottomY, high);
    }
}

} // namespace

void LodMesher::mesh(const LodTile& tile, const LodMeshOptions& options, LodMesh& out) {
    VXE_PROFILE_FUNCTION();
    m_raw.clear();
    m_sides.clear();
    addTops(tile);
    addSides(tile, options);
    mergeSides();
    out.key = tile.key();
    pack(m_raw, out);
}

// The tops of the highest spans, merged greedily: grow a rectangle along X
// while the next cell has the same top, then along Z while whole rows do.
void LodMesher::addTops(const LodTile& tile) {
    m_done.fill(false);
    for (i32 cz = 0; cz < kCells; ++cz) {
        for (i32 cx = 0; cx < kCells; ++cx) {
            const u32 index = cellIndex(cx, cz);
            const std::span<const LodSpan> spans = tile.cellAt(index);
            if (m_done.at(index) || spans.empty()) {
                continue;
            }
            const auto [width, depth] = growTop(tile, cx, cz);
            const LodSpan& top = spans.front();
            m_raw.push_back({.face = kPosY,
                             .x = static_cast<u8>(cx),
                             .z = static_cast<u8>(cz),
                             .sizeU = static_cast<u8>(width),
                             .sizeV = static_cast<u8>(depth),
                             .sky = top.skyLight,
                             .material = top.topMaterial,
                             .y = top.topY + 1});
        }
    }
    addLowerFaces(tile);
}

// The largest rectangle of cells with the same top as cell (cx, cz), not
// taken by an earlier rectangle, starting there; marks its cells taken.
std::pair<i32, i32> LodMesher::growTop(const LodTile& tile, i32 cx, i32 cz) {
    const std::span<const LodSpan> spans = tile.cellAt(cellIndex(cx, cz));
    const auto joins = [&](i32 x, i32 z) {
        return !m_done.at(cellIndex(x, z)) && sameTop(spans, tile.cellAt(cellIndex(x, z)));
    };
    i32 width = 1;
    while (cx + width < kCells && joins(cx + width, cz)) {
        ++width;
    }
    i32 depth = 1;
    const auto rowJoins = [&](i32 z) {
        for (i32 x = cx; x < cx + width; ++x) {
            if (!joins(x, z)) {
                return false;
            }
        }
        return true;
    };
    while (cz + depth < kCells && rowJoins(cz + depth)) {
        ++depth;
    }
    for (i32 z = cz; z < cz + depth; ++z) {
        std::fill_n(m_done.begin() + cellIndex(cx, z), width, true);
    }
    return {width, depth};
}

// Lower spans (rare: overhangs) get one top quad per cell, and every span
// with another span below shows its underside.
void LodMesher::addLowerFaces(const LodTile& tile) {
    for (i32 cz = 0; cz < kCells; ++cz) {
        for (i32 cx = 0; cx < kCells; ++cx) {
            const std::span<const LodSpan> spans = tile.cellAt(cellIndex(cx, cz));
            for (usize i = 0; i < spans.size(); ++i) {
                const LodSpan& span = spans[i];
                const Raw cell{.x = static_cast<u8>(cx), .z = static_cast<u8>(cz), .sky = span.skyLight};
                if (i > 0) {
                    Raw top = cell;
                    top.face = kPosY;
                    top.material = span.topMaterial;
                    top.y = span.topY + 1;
                    m_raw.push_back(top);
                }
                if (i + 1 < spans.size()) {
                    Raw bottom = cell;
                    bottom.face = kNegY;
                    bottom.material = span.sideMaterial;
                    bottom.y = span.bottomY;
                    m_raw.push_back(bottom);
                }
            }
        }
    }
}

// The side of every span towards each neighbor cell, minus what the
// neighbor's spans cover. Beyond the tile's edge the neighbor is unknown, so
// the side reaches down as a skirt instead.
void LodMesher::addSides(const LodTile& tile, const LodMeshOptions& options) {
    struct Direction {
        u8 face;
        i32 dx;
        i32 dz;
    };
    constexpr std::array<Direction, 4> kDirections = {{{.face = kPosX, .dx = 1, .dz = 0},
                                                       {.face = kNegX, .dx = -1, .dz = 0},
                                                       {.face = kPosZ, .dx = 0, .dz = 1},
                                                       {.face = kNegZ, .dx = 0, .dz = -1}}};
    const i64 skirt =
        std::max<i64>(16, static_cast<i64>(std::max(options.skirtCells, 1U)) * tile.key().cellSize());
    for (i32 cz = 0; cz < kCells; ++cz) {
        for (i32 cx = 0; cx < kCells; ++cx) {
            const std::span<const LodSpan> spans = tile.cellAt(cellIndex(cx, cz));
            for (const Direction& direction : kDirections) {
                const i32 nx = cx + direction.dx;
                const i32 nz = cz + direction.dz;
                const bool inside = nx >= 0 && nx < kCells && nz >= 0 && nz < kCells;
                for (const LodSpan& span : spans) {
                    const auto emit = [&](i64 low, i64 high) {
                        m_sides.push_back({.face = direction.face,
                                           .x = static_cast<u8>(cx),
                                           .z = static_cast<u8>(cz),
                                           .sky = span.skyLight,
                                           .material = span.sideMaterial,
                                           .y = static_cast<i32>(low),
                                           .height = static_cast<i32>(high - low + 1)});
                    };
                    if (inside) {
                        forEachOpenInterval(span, tile.cellAt(cellIndex(nx, nz)), emit);
                    } else {
                        emit(std::max<i64>(span.bottomY, i64{span.topY} - skirt + 1), span.topY);
                    }
                }
            }
        }
    }
}

// Joins side quads that continue each other along their edge (same face,
// plane, heights, material and light, neighboring cells), up to 64 cells.
void LodMesher::mergeSides() {
    const auto alongX = [](const Raw& raw) { return raw.face == kPosZ || raw.face == kNegZ; };
    const auto plane = [&](const Raw& raw) { return alongX(raw) ? raw.z : raw.x; };
    const auto along = [&](const Raw& raw) { return alongX(raw) ? raw.x : raw.z; };
    // Sorted so the quads that can join sit next to each other, in order
    // along their edge.
    const auto key = [&](const Raw& raw) {
        return std::tuple(raw.face, plane(raw), raw.y, raw.height, raw.material, raw.sky, along(raw));
    };
    std::ranges::sort(m_sides, [&](const Raw& a, const Raw& b) { return key(a) < key(b); });
    for (usize i = 0; i < m_sides.size();) {
        Raw merged = m_sides[i];
        usize next = i + 1;
        while (next < m_sides.size()) {
            const Raw& candidate = m_sides[next];
            const bool continues = candidate.face == merged.face && candidate.y == merged.y &&
                                   candidate.height == merged.height &&
                                   candidate.material == merged.material && candidate.sky == merged.sky &&
                                   plane(candidate) == plane(merged) &&
                                   along(candidate) == along(merged) + merged.sizeU && merged.sizeU < kCells;
            if (!continues) {
                break;
            }
            ++merged.sizeU;
            ++next;
        }
        m_raw.push_back(merged);
        i = next;
    }
}

void LodMesher::pack(std::span<const Raw> raws, LodMesh& out) {
    out.quads.clear();
    if (raws.empty()) {
        out.baseY = out.minY = out.maxY = 0;
        return;
    }
    i64 low = std::numeric_limits<i64>::max();
    i64 high = std::numeric_limits<i64>::min();
    for (const Raw& raw : raws) {
        low = std::min<i64>(low, raw.y);
        high = std::max<i64>(high, i64{raw.y} + raw.height);
    }
    out.baseY = static_cast<i32>(low);
    out.minY = static_cast<i32>(low);
    out.maxY = static_cast<i32>(high);
    out.quads.reserve(raws.size());
    for (const Raw& raw : raws) {
        const auto relative = static_cast<u32>(std::min<i64>(i64{raw.y} - low, kLodMaxRelativeY));
        const auto height = static_cast<u32>(std::min<i64>(raw.height, kLodMaxHeight));
        LodQuad quad;
        quad.words[0] = u32{raw.x} | (u32{raw.z} << 7U) | (u32{raw.sizeU - 1U} << 14U) |
                        (u32{raw.sizeV - 1U} << 20U) | (u32{raw.face} << 26U);
        quad.words[1] = relative | (u32{raw.sky} << 24U);
        quad.words[2] = height | (std::min<u32>(raw.material, kLodMaxMaterial) << 20U);
        out.quads.push_back(quad);
    }
}

} // namespace vxe::voxel
