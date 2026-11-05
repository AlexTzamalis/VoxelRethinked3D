#include "vxe/voxel/mesher.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>
#include <bit>
#include <utility>

namespace vxe::voxel {
namespace {

constexpr usize kRow = kSnapshotSize;      // Snapshot rows along X: index y * 18 + z.
constexpr usize kSlice = kSnapshotArea;    // Snapshot states: y * 324 + z * 18 + x.
constexpr u32 kInterior = 0x1FFFEU;        // Bits 1 to 16 of a row: the chunk's own blocks.
constexpr u32 kKindMask = 3U;              // Kind in the low bits of a state's class byte...
constexpr u8 kPlantBit = 8U;               // ...plus "adds crossed planes" (plants, bushy leaves).
constexpr u8 kCullsSameBit = 4U;           // ...plus "hides faces against its own state".
constexpr u8 kShapeBit = 16U;              // ...plus "a slab with quads of its own".
constexpr u64 kLightBits = 0xFFULL << 22U; // Sky and block light in word1.

constexpr std::array<BlockFace, kBlockFaceCount> kFaces = {BlockFace::PosX, BlockFace::NegX, BlockFace::PosY,
                                                           BlockFace::NegY, BlockFace::PosZ, BlockFace::NegZ};

// The running bounds of a mesh in block corners.
struct Bounds {
    std::array<u32, 3> min = {16, 16, 16};
    std::array<u32, 3> max = {0, 0, 0};
};

// Packs a merged rectangle of faces into a quad and grows the bounds.
void appendQuad(BlockFace face, u32 slice, u32 u, u32 v, u32 width, u32 height, u32 word1,
                std::vector<PackedQuad>& quads, Bounds& bounds) {
    const FaceAxes axes = faceAxes(face);
    const u32 ao0 = (word1 >> 14U) & 3U;
    const u32 ao1 = (word1 >> 16U) & 3U;
    const u32 ao2 = (word1 >> 18U) & 3U;
    const u32 ao3 = (word1 >> 20U) & 3U;
    // Split the quad along the diagonal through its darker pair of corners;
    // the other diagonal would show a hard crease of shading across the face
    // (Section 10.3).
    const u32 flip = (ao0 + ao2) > (ao1 + ao3) ? 1U : 0U;
    std::array<u32, 3> origin{};
    origin[axes.normal] = slice + (axes.positive ? 1U : 0U);
    origin[axes.u] = u;
    origin[axes.v] = v;
    std::array<u32, 3> extent = origin;
    extent[axes.u] += width;
    extent[axes.v] += height;
    for (usize axis = 0; axis < 3; ++axis) {
        bounds.min[axis] = std::min(bounds.min[axis], origin[axis]);
        bounds.max[axis] = std::max(bounds.max[axis], extent[axis]);
    }
    const u32 word0 = origin[0] | (origin[1] << 5U) | (origin[2] << 10U) | ((width - 1U) << 15U) |
                      ((height - 1U) << 19U) | (static_cast<u32>(face) << 23U) | (flip << 26U);
    quads.push_back({.word0 = word0, .word1 = word1});
}

// The chunk faces (one bit per BlockFace) that a filled run of open blocks
// in row `row` (y * 16 + z) touches.
u32 facesTouched(usize row, u32 fill) noexcept {
    const usize y = row / 16;
    const usize z = row % 16;
    const auto bit = [](BlockFace face) { return 1U << static_cast<u32>(face); };
    return ((fill & 1U) != 0 ? bit(BlockFace::NegX) : 0U) |
           ((fill & 0x8000U) != 0 ? bit(BlockFace::PosX) : 0U) | (y == 0 ? bit(BlockFace::NegY) : 0U) |
           (y == 15 ? bit(BlockFace::PosY) : 0U) | (z == 0 ? bit(BlockFace::NegZ) : 0U) |
           (z == 15 ? bit(BlockFace::PosZ) : 0U);
}

// Every face pair of a set of touched faces, as connectivity bits.
u16 pairsOf(u32 touched) noexcept {
    u16 result = 0;
    for (usize a = 0; a < kBlockFaceCount; ++a) {
        for (usize b = a + 1; b < kBlockFaceCount; ++b) {
            if (((touched >> a) & (touched >> b) & 1U) != 0) {
                result = static_cast<u16>(result | connectivityBit(kFaces[a], kFaces[b]));
            }
        }
    }
    return result;
}

} // namespace

MesherSettings::MesherSettings(config::SettingsRegistry& settings)
    : m_greedyMerge(settings.registerBool(
          {{.id = "Graphics.Terrain.GreedyMerge",
            .displayName = "Greedy merging",
            .description = "Merges equal neighboring block faces into larger quads. Off is for debugging.",
            .applyMode = config::ApplyMode::RequiresRemesh,
            .visibility = config::Visibility::Advanced},
           true}))
    , m_mergeAcrossLight(settings.registerBool(
          {{.id = "Graphics.Terrain.MergeAcrossLight",
            .displayName = "Merge across light",
            .description = "Merges faces even when their light differs: fewer quads, blockier light.",
            .applyMode = config::ApplyMode::RequiresRemesh,
            .visibility = config::Visibility::Advanced},
           false}))
    , m_fancyLeaves(settings.registerBool(
          {{.id = "Graphics.Terrain.FancyLeaves",
            .displayName = "Fancy leaves",
            .description = "See through leaves. Off draws them as solid blocks, which is faster.",
            .applyMode = config::ApplyMode::RequiresRemesh},
           true}))
    , m_advancedLeaves(settings.registerBool(
          {{.id = "Graphics.Terrain.AdvancedLeaves",
            .displayName = "Advanced leaves",
            .description = "Leaves grow bushy shapes past their blocks, for fuller trees.",
            .applyMode = config::ApplyMode::RequiresRemesh},
           false})) {}

MeshOptions MesherSettings::options() const {
    return {.greedyMerge = m_greedyMerge.get(),
            .mergeAcrossLight = m_mergeAcrossLight.get(),
            .fancyLeaves = m_fancyLeaves.get(),
            .advancedLeaves = m_advancedLeaves.get()};
}

Mesher::Mesher(stats::StatRegistry* stats)
    : m_classes(kMaxBlockStates, u8{0})
    , m_heights(kMaxBlockStates, u8{0})
    , m_stats(stats) {
    m_floodStack.reserve(1024);
    if (m_stats != nullptr) {
        const auto add = [this](const stats::StatDesc& desc) {
            auto id = m_stats->registerStat(desc);
            return id ? id.value() : stats::StatId{};
        };
        m_chunksStat = add({.name = "Voxel.Mesher.Chunks", .description = "Chunks meshed this frame."});
        m_quadsStat = add({.name = "Voxel.Mesher.Quads", .description = "Quads produced this frame."});
        m_timeStat = add({.name = "Voxel.Mesher.Time",
                          .description = "Time spent meshing this frame, over all threads.",
                          .type = stats::StatType::Timer,
                          .unit = stats::StatUnit::Milliseconds});
    }
}

void Mesher::prepare(const BlockRegistryView& blocks, const MeshOptions& options) {
    // Views that went through BlockRegistry::publish have a unique
    // generation; unpublished ones (generation 0, tests and tools) are
    // classified every time, which costs microseconds for small tables.
    if (blocks.generation() != 0 && &blocks == m_preparedView &&
        blocks.generation() == m_preparedGeneration && options == m_preparedOptions) {
        return;
    }
    // Every possible state has a class byte, so the hot loop needs no bounds
    // check; states the view does not know stay invisible like air. Only
    // the bytes the last view set need clearing.
    std::fill_n(m_classes.begin(), static_cast<std::ptrdiff_t>(m_preparedSize), u8{0});
    std::fill_n(m_heights.begin(), static_cast<std::ptrdiff_t>(m_preparedSize), u8{0});
    m_preparedSize = std::min(blocks.size(), m_classes.size());
    for (usize state = 0; state < m_preparedSize; ++state) {
        const BlockRenderInfo& info = blocks.info(static_cast<BlockStateId>(state));
        Kind kind = Kind::Invisible;
        switch (info.layer) {
        case RenderLayer::Opaque:
            kind = Kind::Opaque;
            break;
        case RenderLayer::Cutout:
            kind = options.fancyLeaves ? Kind::Cutout : Kind::Opaque;
            break;
        case RenderLayer::Translucent:
            kind = Kind::Translucent;
            break;
        case RenderLayer::Plant:
        case RenderLayer::Invisible:
            break;
        }
        u8 value = static_cast<u8>(kind);
        if (info.layer == RenderLayer::Plant || (options.advancedLeaves && info.bushyLayer != 0)) {
            value = static_cast<u8>(value | kPlantBit);
        }
        if (kind == Kind::Cutout || kind == Kind::Translucent) {
            value = static_cast<u8>(value | (info.cullsSameType ? kCullsSameBit : 0U));
        }
        if (info.height != 0 && info.isVisible() && info.layer != RenderLayer::Plant) {
            // A slab is in no mask: it hides nothing, and its faces come from addShapes.
            value = kShapeBit;
            m_heights[state] = std::min<u8>(info.height, 15);
        }
        m_classes[state] = value;
    }
    m_preparedView = &blocks;
    m_preparedGeneration = blocks.generation();
    m_preparedOptions = options;
}

void Mesher::buildMasks(const ChunkSnapshot& snapshot) {
    // One pass over the 5832 states: each row of 18 becomes four masks,
    // built in registers and stored once per row. Most rows of real terrain
    // are one state (air above ground, stone below), which the first check
    // turns into a single lookup.
    const BlockStateId* states = snapshot.states.data();
    const u8* classes = m_classes.data();
    m_hasSeeThrough = false;
    u32 seen = 0;
    for (usize row = 0; row < kSnapshotArea; ++row) {
        const BlockStateId* line = states + ((row / kRow) * kSlice) + ((row % kRow) * kRow);
        u32 opaque = 0;
        u32 cutout = 0;
        u32 translucent = 0;
        u32 cullsSame = 0;
        bool uniform = true;
        for (u32 x = 1; x < kSnapshotSize; ++x) {
            uniform = uniform && line[x] == line[0];
        }
        if (uniform) {
            const u32 value = classes[line[0]];
            const u32 kind = value & kKindMask;
            const u32 full = (1U << kSnapshotSize) - 1U;
            seen |= value;
            opaque = kind == static_cast<u32>(Kind::Opaque) ? full : 0U;
            cutout = kind == static_cast<u32>(Kind::Cutout) ? full : 0U;
            translucent = kind == static_cast<u32>(Kind::Translucent) ? full : 0U;
            cullsSame = (value & kCullsSameBit) != 0 ? full : 0U;
        } else {
            for (u32 x = 0; x < kSnapshotSize; ++x) {
                const u32 value = classes[line[x]];
                const u32 kind = value & kKindMask;
                seen |= value;
                opaque |= static_cast<u32>(kind == static_cast<u32>(Kind::Opaque)) << x;
                cutout |= static_cast<u32>(kind == static_cast<u32>(Kind::Cutout)) << x;
                translucent |= static_cast<u32>(kind == static_cast<u32>(Kind::Translucent)) << x;
                cullsSame |= ((value & kCullsSameBit) >> 2U) << x;
            }
        }
        m_opaque[row] = opaque;
        m_cutout[row] = cutout;
        m_translucent[row] = translucent;
        m_cullsSame[row] = cullsSame;
        m_hasSeeThrough = m_hasSeeThrough || ((cutout | translucent) != 0);
    }
    m_hasPlants = (seen & kPlantBit) != 0;
    m_hasShapes = (seen & kShapeBit) != 0;
}

void Mesher::buildSameStateMasks(const ChunkSnapshot& snapshot) {
    // For see through blocks a face also hides against the same state when
    // that state culls its own type. These masks say, per row, which blocks
    // equal their neighbor after them along X (bit x: x and x + 1), above
    // them (y + 1) and behind them (z + 1). Only rows near see through
    // blocks are compared; the rest stay zero.
    const BlockStateId* states = snapshot.states.data();
    const auto equalMask = [](const BlockStateId* a, const BlockStateId* b) {
        u32 mask = 0;
        for (u32 x = 0; x < kSnapshotSize; ++x) {
            mask |= static_cast<u32>(a[x] == b[x]) << x;
        }
        return mask;
    };
    for (usize y = 0; y + 1 < kRow; ++y) {
        for (usize z = 0; z + 1 < kRow; ++z) {
            const usize row = (y * kRow) + z;
            const u32 here = m_cutout[row] | m_translucent[row];
            const u32 above = m_cutout[row + kRow] | m_translucent[row + kRow];
            const u32 behind = m_cutout[row + 1] | m_translucent[row + 1];
            const BlockStateId* line = states + (y * kSlice) + (z * kRow);
            m_sameX[row] = here != 0 ? equalMask(line, line + 1) : 0U;
            m_sameY[row] = (here | above) != 0 ? equalMask(line, line + kSlice) : 0U;
            m_sameZ[row] = (here | behind) != 0 ? equalMask(line, line + kRow) : 0U;
        }
    }
}

void Mesher::addFaces(usize layer, BlockFace face, usize y, usize z, u32 bits) {
    // `bits` are snapshot x positions (1 to 16) of visible faces in the row
    // at snapshot (y, z). Y and Z faces keep the row as is (their U axis is
    // X); X faces turn it into one bit per slice (their U axis is Z).
    bits &= kInterior;
    if (bits == 0) {
        return;
    }
    const auto f = static_cast<usize>(face);
    auto& slices = m_faces[layer][f];
    const usize group = (layer * kBlockFaceCount) + f;
    switch (face) {
    case BlockFace::PosY:
    case BlockFace::NegY:
        slices[y - 1][z - 1] = static_cast<u16>(bits >> 1U);
        m_groupSlices[group] |= 1U << (y - 1);
        break;
    case BlockFace::PosZ:
    case BlockFace::NegZ:
        slices[z - 1][y - 1] = static_cast<u16>(bits >> 1U);
        m_groupSlices[group] |= 1U << (z - 1);
        break;
    case BlockFace::PosX:
    case BlockFace::NegX:
        while (bits != 0) {
            const u32 x = static_cast<u32>(std::countr_zero(bits));
            bits &= bits - 1;
            slices[x - 1][y - 1] = static_cast<u16>(slices[x - 1][y - 1] | (1U << (z - 1)));
            m_groupSlices[group] |= 1U << (x - 1);
        }
        break;
    }
}

void Mesher::findFaces() {
    // A face is visible where its block is drawn and the neighbor in that
    // direction does not hide it. Whole rows at a time: shifting a row by
    // one lines every block up with its X neighbor; the rows at y +- 1 and
    // z +- 1 are the Y and Z neighbors.
    for (usize y = 1; y <= kChunkSize; ++y) {
        for (usize z = 1; z <= kChunkSize; ++z) {
            const usize row = (y * kRow) + z;
            const u32 opaque = m_opaque[row];
            if ((opaque & kInterior) != 0) {
                const auto layer = static_cast<usize>(MeshLayer::Opaque);
                addFaces(layer, BlockFace::PosX, y, z, opaque & ~(opaque >> 1U));
                addFaces(layer, BlockFace::NegX, y, z, opaque & ~(opaque << 1U));
                addFaces(layer, BlockFace::PosY, y, z, opaque & ~m_opaque[row + kRow]);
                addFaces(layer, BlockFace::NegY, y, z, opaque & ~m_opaque[row - kRow]);
                addFaces(layer, BlockFace::PosZ, y, z, opaque & ~m_opaque[row + 1]);
                addFaces(layer, BlockFace::NegZ, y, z, opaque & ~m_opaque[row - 1]);
            }
            if (!m_hasSeeThrough) {
                continue;
            }
            // Cutout and translucent blocks: hidden by opaque neighbors and,
            // if their state culls its own type, by the same state.
            const u32 cull = m_cullsSame[row];
            const std::array<u32, kBlockFaceCount> hidden = {
                (opaque >> 1U) | (m_sameX[row] & cull),
                (opaque << 1U) | ((m_sameX[row] << 1U) & cull),
                m_opaque[row + kRow] | (m_sameY[row] & cull),
                m_opaque[row - kRow] | (m_sameY[row - kRow] & cull),
                m_opaque[row + 1] | (m_sameZ[row] & cull),
                m_opaque[row - 1] | (m_sameZ[row - 1] & cull),
            };
            const std::array<std::pair<MeshLayer, u32>, 2> layers = {
                std::pair{MeshLayer::Cutout, m_cutout[row]},
                std::pair{MeshLayer::Translucent, m_translucent[row]}};
            for (const auto& [layer, blocks] : layers) {
                if ((blocks & kInterior) == 0) {
                    continue;
                }
                for (usize f = 0; f < kBlockFaceCount; ++f) {
                    addFaces(static_cast<usize>(layer), kFaces[f], y, z, blocks & ~hidden[f]);
                }
            }
        }
    }
}

u64 Mesher::faceKey(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks, BlockFace face, u32 slice,
                    u32 u, u32 v) const noexcept {
    // Positions in the opaque masks are (row, bit) = (y * 18 + z, x). Moving
    // along X changes the bit, along Y the row by 18, along Z the row by 1,
    // so each face's U and V axes are a fixed row and bit step, and the 8
    // AO neighbors are 8 shifted reads with no 3D position math.
    constexpr std::array<i32, 3> kRowStep = {0, static_cast<i32>(kRow), 1};
    constexpr std::array<i32, 3> kBitStep = {1, 0, 0};
    const FaceAxes axes = faceAxes(face);
    std::array<i32, 3> block{};
    block[axes.normal] = static_cast<i32>(slice) + 1;
    block[axes.u] = static_cast<i32>(u) + 1;
    block[axes.v] = static_cast<i32>(v) + 1;
    const BlockStateId state =
        snapshot.states[(static_cast<usize>(block[1]) * kSlice) + (static_cast<usize>(block[2]) * kRow) +
                        static_cast<usize>(block[0])];
    // The empty cell in front of the face.
    const i32 toFront = axes.positive ? 1 : -1;
    const i32 frontRow = (block[1] * static_cast<i32>(kRow)) + block[2] + (toFront * kRowStep[axes.normal]);
    const i32 frontBit = block[0] + (toFront * kBitStep[axes.normal]);
    const i32 rowU = kRowStep[axes.u];
    const i32 bitU = kBitStep[axes.u];
    const i32 rowV = kRowStep[axes.v];
    const i32 bitV = kBitStep[axes.v];
    const auto opaqueAt = [&](i32 du, i32 dv) -> u32 {
        const i32 row = frontRow + (du * rowU) + (dv * rowV);
        const i32 bit = frontBit + (du * bitU) + (dv * bitV);
        return (m_opaque[static_cast<usize>(row)] >> static_cast<u32>(bit)) & 1U;
    };
    // Classic voxel AO (Section 10.3): two sides and the corner between
    // them; both sides closed is fully dark whatever the corner says.
    const u32 left = opaqueAt(-1, 0);
    const u32 right = opaqueAt(1, 0);
    const u32 down = opaqueAt(0, -1);
    const u32 up = opaqueAt(0, 1);
    const auto cornerAo = [](u32 sideU, u32 sideV, u32 corner) -> u32 {
        return sideU != 0 && sideV != 0 ? 0U : 3U - (sideU + sideV + corner);
    };
    const u32 ao = cornerAo(left, down, opaqueAt(-1, -1)) | (cornerAo(right, down, opaqueAt(1, -1)) << 2U) |
                   (cornerAo(right, up, opaqueAt(1, 1)) << 4U) | (cornerAo(left, up, opaqueAt(-1, 1)) << 6U);
    const u32 layer = std::min<u32>(blocks.info(state).textureLayer(face), 0x3FFFU);
    // A face is lit by the open cell in front of it (Section 14): its sky
    // light in bits 22 to 25, its block light in 26 to 29.
    const usize front = (static_cast<usize>(frontRow) * kRow) + static_cast<usize>(frontBit);
    const u8 light = snapshot.light[front];
    const u32 word1 =
        layer | (ao << 14U) | (u32{skyLightOf(light)} << 22U) | (u32{blockLightOf(light)} << 26U);
    return u64{word1} | (u64{state} << 32U);
}

Mesher::Rectangle Mesher::growRectangle(const MergeInput& in, BlockFace face, u32 slice, FaceRows& rows,
                                        u32 u, u32 v) const noexcept {
    const u64 key = faceKey(in.snapshot, in.blocks, face, slice, u, v);
    const u64 match = key & in.compareMask;
    const auto matches = [&](u32 du, u32 dv) {
        return (faceKey(in.snapshot, in.blocks, face, slice, du, dv) & in.compareMask) == match;
    };
    Rectangle rectangle{.width = 1, .height = 1, .word1 = static_cast<u32>(key)};
    if (in.greedy) {
        // Grow right while the next face exists and matches.
        while (u + rectangle.width < 16 && ((rows[v] >> (u + rectangle.width)) & 1U) != 0 &&
               matches(u + rectangle.width, v)) {
            ++rectangle.width;
        }
    }
    const auto run = static_cast<u16>(((1U << rectangle.width) - 1U) << u);
    rows[v] = static_cast<u16>(rows[v] & ~run);
    if (!in.greedy) {
        return rectangle;
    }
    // Grow up while the whole run exists and matches in the next row.
    while (v + rectangle.height < 16 && (rows[v + rectangle.height] & run) == run) {
        const u32 next = v + rectangle.height;
        bool same = true;
        for (u32 du = 0; du < rectangle.width && same; ++du) {
            same = matches(u + du, next);
        }
        if (!same) {
            break;
        }
        rows[next] = static_cast<u16>(rows[next] & ~run);
        ++rectangle.height;
    }
    return rectangle;
}

void Mesher::mergeFaces(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks,
                        const MeshOptions& options, ChunkMesh& out) {
    // Light bits are left out of the comparison when merging across light.
    const MergeInput in{.snapshot = snapshot,
                        .blocks = blocks,
                        .compareMask = options.mergeAcrossLight ? ~kLightBits : ~u64{0},
                        .greedy = options.greedyMerge};
    Bounds bounds;
    const usize plantGroup = faceGroupIndex(MeshLayer::Plants, BlockFace::PosX);
    if (m_hasShapes) {
        addShapes(snapshot, blocks);
    }
    for (usize group = 0; group < kFaceGroupCount; ++group) {
        out.groupBegin[group] = static_cast<u32>(out.quads.size());
        if (group == plantGroup && m_hasPlants) {
            addPlants(snapshot, blocks, options, out);
            if (out.quads.size() > out.groupBegin[group]) {
                // Whole cells; bushy planes reach past them, so a chunk with
                // plants keeps the full box and culling never clips them.
                bounds.min = {0, 0, 0};
                bounds.max = {16, 16, 16};
            }
        }
        u32 slices = m_groupSlices[group];
        m_groupSlices[group] = 0;
        const BlockFace face = kFaces[group % kBlockFaceCount];
        if (group / kBlockFaceCount == static_cast<usize>(MeshLayer::Opaque) && m_hasShapes) {
            std::vector<PackedQuad>& shapes = m_shapeQuads[group % kBlockFaceCount];
            for (const PackedQuad& quad : shapes) {
                // Whole cells for the bounds: a slab never reaches past its own.
                std::array<u32, 3> cell = {quad.x(), quad.y(), quad.z()};
                const FaceAxes axes = faceAxes(face);
                cell[axes.normal] -= axes.positive ? 1U : 0U;
                for (usize axis = 0; axis < 3; ++axis) {
                    bounds.min[axis] = std::min(bounds.min[axis], cell[axis]);
                    bounds.max[axis] = std::max(bounds.max[axis], cell[axis] + 1U);
                }
            }
            out.quads.insert(out.quads.end(), shapes.begin(), shapes.end());
            shapes.clear();
        }
        while (slices != 0) {
            const auto slice = static_cast<u32>(std::countr_zero(slices));
            slices &= slices - 1;
            FaceRows& rows = m_faces[group / kBlockFaceCount][static_cast<usize>(face)][slice];
            for (u32 v = 0; v < 16; ++v) {
                while (rows[v] != 0) {
                    // The first face left in this row starts a rectangle.
                    const auto u = static_cast<u32>(std::countr_zero(static_cast<u32>(rows[v])));
                    const Rectangle r = growRectangle(in, face, slice, rows, u, v);
                    appendQuad(face, slice, u, v, r.width, r.height, r.word1, out.quads, bounds);
                }
            }
        }
    }
    out.groupBegin.back() = static_cast<u32>(out.quads.size());
    for (usize axis = 0; axis < 3; ++axis) {
        out.boundsMin[axis] = static_cast<u8>(bounds.min[axis]);
        out.boundsMax[axis] = static_cast<u8>(bounds.max[axis]);
    }
}

u32 Mesher::floodComponent(usize start, const OpenRows& open, OpenRows& visited) {
    // Fills one component of open blocks, starting at the lowest unvisited
    // open block of row `start`, and returns the chunk faces it touches.
    const auto push = [&](usize row, u32 bits) {
        if ((bits & open[row] & ~static_cast<u32>(visited[row])) != 0) {
            m_floodStack.push_back(static_cast<u32>(row) | (bits << 16U));
        }
    };
    const auto unvisited = static_cast<u32>(open[start] & ~visited[start]);
    push(start, 1U << static_cast<u32>(std::countr_zero(unvisited)));
    u32 touched = 0;
    while (!m_floodStack.empty()) {
        const u32 entry = m_floodStack.back();
        m_floodStack.pop_back();
        const usize row = entry & 0xFFFFU;
        const u32 rowOpen = open[row];
        u32 fill = (entry >> 16U) & rowOpen & ~static_cast<u32>(visited[row]);
        if (fill == 0) {
            continue;
        }
        // Spread along the row through open blocks.
        for (u32 grown = (fill | (fill << 1U) | (fill >> 1U)) & rowOpen; grown != fill;
             grown = (fill | (fill << 1U) | (fill >> 1U)) & rowOpen) {
            fill = grown;
        }
        fill &= ~static_cast<u32>(visited[row]);
        visited[row] = static_cast<u16>(visited[row] | fill);
        touched |= facesTouched(row, fill);
        // Then into the four neighboring rows (y +- 1, z +- 1).
        const usize y = row / 16;
        const usize z = row % 16;
        if (y > 0) {
            push(row - 16, fill);
        }
        if (y < 15) {
            push(row + 16, fill);
        }
        if (z > 0) {
            push(row - 1, fill);
        }
        if (z < 15) {
            push(row + 1, fill);
        }
    }
    return touched;
}

void Mesher::addPlants(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks,
                       const MeshOptions& options, ChunkMesh& out) {
    const i64 originX = i64{snapshot.position.x} * kChunkSize;
    const i64 originZ = i64{snapshot.position.z} * kChunkSize;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 z = 0; z < 16; ++z) {
            const usize line = (usize{y + 1} * kSlice) + (usize{z + 1} * kRow);
            for (u32 x = 0; x < 16; ++x) {
                const usize index = line + x + 1;
                const BlockStateId state = snapshot.states[index];
                if ((m_classes[state] & kPlantBit) == 0) {
                    continue;
                }
                const BlockRenderInfo& info = blocks.info(state);
                const bool plant = info.layer == RenderLayer::Plant;
                u32 extra = 0;
                u32 layer = 0;
                if (plant) {
                    // Only X and Z, so both halves of a tall plant agree; the
                    // seed gives each world its own pattern.
                    const u64 hash =
                        hashCombine(hashCombine(options.variationSeed, static_cast<u64>(originX + x)),
                                    static_cast<u64>(originZ + z));
                    layer = info.textureLayer(BlockFace::PosX) + static_cast<u32>(hash % info.variationCount);
                    extra = static_cast<u32>((hash >> 32U) & 15U) << 27U;
                } else {
                    // Buried leaves show nothing: only those with a free side.
                    const u32 row =
                        m_opaque[(usize{y + 1} * kRow) + z + 1] | m_cutout[(usize{y + 1} * kRow) + z + 1];
                    const u32 bit = 1U << (x + 1);
                    const bool buried =
                        (row & (bit << 1U)) != 0 && (row & (bit >> 1U)) != 0 &&
                        ((m_opaque[(usize{y + 2} * kRow) + z + 1] | m_cutout[(usize{y + 2} * kRow) + z + 1]) &
                         bit) != 0 &&
                        ((m_opaque[(usize{y} * kRow) + z + 1] | m_cutout[(usize{y} * kRow) + z + 1]) & bit) !=
                            0 &&
                        ((m_opaque[(usize{y + 1} * kRow) + z + 2] | m_cutout[(usize{y + 1} * kRow) + z + 2]) &
                         bit) != 0 &&
                        ((m_opaque[(usize{y + 1} * kRow) + z] | m_cutout[(usize{y + 1} * kRow) + z]) & bit) !=
                            0;
                    if (buried) {
                        continue;
                    }
                    layer = info.bushyLayer - 1U;
                    // Its own turn, so neighbors' planes are never parallel.
                    const u64 turn = hashCombine(
                        hashCombine(hashCombine(options.variationSeed, static_cast<u64>(originX + x)), y),
                        static_cast<u64>(originZ + z));
                    extra = (1U << 31U) | (static_cast<u32>((turn >> 32U) & 15U) << 27U);
                }
                const u8 light = snapshot.light[index];
                const u32 word1 = layer | (0xFFU << 14U) | (u32{skyLightOf(light)} << 22U) |
                                  (u32{blockLightOf(light)} << 26U);
                const u32 position = x | (y << 5U) | (z << 10U) | extra;
                out.quads.push_back({.word0 = position | (6U << 23U), .word1 = word1});
                out.quads.push_back({.word0 = position | (7U << 23U), .word1 = word1});
            }
        }
    }
}

void Mesher::addShapes(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks) {
    // Slabs are few (snow on the ground), so each one is looked at on its own:
    // up to six single block quads, with its height in word0 bits 27 to 30. A
    // side is hidden by an opaque neighbor or a slab at least as tall; the top
    // never touches the block above, so it always shows.
    constexpr std::array<std::ptrdiff_t, kBlockFaceCount> kStep = {1,
                                                                   -1,
                                                                   static_cast<std::ptrdiff_t>(kSlice),
                                                                   -static_cast<std::ptrdiff_t>(kSlice),
                                                                   static_cast<std::ptrdiff_t>(kRow),
                                                                   -static_cast<std::ptrdiff_t>(kRow)};
    for (u32 y = 0; y < 16; ++y) {
        for (u32 z = 0; z < 16; ++z) {
            const usize line = (usize{y + 1} * kSlice) + (usize{z + 1} * kRow);
            for (u32 x = 0; x < 16; ++x) {
                const usize index = line + x + 1;
                const BlockStateId state = snapshot.states[index];
                if ((m_classes[state] & kShapeBit) == 0) {
                    continue;
                }
                const BlockRenderInfo& info = blocks.info(state);
                const u32 height = m_heights[state];
                for (usize f = 0; f < kBlockFaceCount; ++f) {
                    const BlockFace face = kFaces[f];
                    const auto neighbor =
                        static_cast<usize>(static_cast<std::ptrdiff_t>(index) + kStep.at(f));
                    const BlockStateId next = snapshot.states[neighbor];
                    const bool opaque = (m_classes[next] & kKindMask) == static_cast<u32>(Kind::Opaque);
                    const bool side = face != BlockFace::PosY && face != BlockFace::NegY;
                    const bool hidden =
                        face != BlockFace::PosY &&
                        (opaque || (side && (m_classes[next] & kShapeBit) != 0 && m_heights[next] >= height));
                    if (hidden) {
                        continue;
                    }
                    const u8 light = snapshot.light[face == BlockFace::PosY ? index : neighbor];
                    const u32 word1 = u32{info.textureLayer(face)} | (0xFFU << 14U) |
                                      (u32{skyLightOf(light)} << 22U) | (u32{blockLightOf(light)} << 26U);
                    std::array<u32, 3> origin = {x, y, z};
                    const FaceAxes axes = faceAxes(face);
                    origin[axes.normal] += axes.positive ? 1U : 0U;
                    const u32 word0 = origin[0] | (origin[1] << 5U) | (origin[2] << 10U) |
                                      (static_cast<u32>(face) << 23U) | (height << 27U);
                    m_shapeQuads.at(f).push_back({.word0 = word0, .word1 = word1});
                }
            }
        }
    }
}

u16 Mesher::connectivity() {
    // Which chunk faces can see each other through blocks that are not
    // opaque (Section 8.3). Rows of 16 open blocks along X are bit masks; a
    // flood fill spreads along a row with shifts and to the four
    // neighboring rows with ANDs, so it touches rows, not single blocks.
    OpenRows open{};
    bool allOpen = true;
    bool allClosed = true;
    for (usize y = 0; y < 16; ++y) {
        for (usize z = 0; z < 16; ++z) {
            const u32 row = ~m_opaque[((y + 1) * kRow) + z + 1];
            const auto bits = static_cast<u16>((row & kInterior) >> 1U);
            open[(y * 16) + z] = bits;
            allOpen = allOpen && bits == 0xFFFF;
            allClosed = allClosed && bits == 0;
        }
    }
    if (allOpen) {
        return kAllConnected;
    }
    if (allClosed) {
        return 0;
    }
    OpenRows visited{};
    u16 result = 0;
    for (usize start = 0; start < kChunkArea; ++start) {
        while ((open[start] & ~visited[start]) != 0) {
            result = static_cast<u16>(result | pairsOf(floodComponent(start, open, visited)));
        }
    }
    return result;
}

void Mesher::mesh(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks, const MeshOptions& options,
                  ChunkMesh& out) {
    VXE_PROFILE_FUNCTION();
    const Stopwatch stopwatch;
    prepare(blocks, options);
    buildMasks(snapshot);
    if (m_hasSeeThrough) {
        buildSameStateMasks(snapshot);
    }
    findFaces();
    out.position = snapshot.position;
    out.version = snapshot.version;
    out.quads.clear();
    mergeFaces(snapshot, blocks, options, out);
    out.connectivity = connectivity();
    if (m_stats != nullptr && m_chunksStat.isValid() && m_quadsStat.isValid() && m_timeStat.isValid()) {
        m_stats->add(m_chunksStat);
        m_stats->add(m_quadsStat, static_cast<i64>(out.quads.size()));
        m_stats->addTime(m_timeStat, stopwatch.elapsed());
    }
}

void sortBackToFront(std::span<PackedQuad> quads, const Vec3& viewer) {
    const auto distance = [&](const PackedQuad& quad) {
        const FaceAxes axes = faceAxes(quad.face());
        Vec3 center(static_cast<f32>(quad.x()), static_cast<f32>(quad.y()), static_cast<f32>(quad.z()));
        center[static_cast<glm::length_t>(axes.u)] += 0.5F * static_cast<f32>(quad.width());
        center[static_cast<glm::length_t>(axes.v)] += 0.5F * static_cast<f32>(quad.height());
        const Vec3 delta = center - viewer;
        return (delta.x * delta.x) + (delta.y * delta.y) + (delta.z * delta.z);
    };
    std::ranges::sort(quads,
                      [&](const PackedQuad& a, const PackedQuad& b) { return distance(a) > distance(b); });
}

} // namespace vxe::voxel
