#include "terrain.hpp"

#include <vxe/core/hash.hpp>
#include <vxe/math/noise.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

namespace vr {

using vxe::f32;
using vxe::f64;
using vxe::i32;
using vxe::i64;
using vxe::u32;
using vxe::u64;
using vxe::u8;
using vxe::usize;
using vxe::noise::smooth;
using vxe::noise::unit;
using vxe::voxel::BlockStateId;

namespace {

constexpr i64 kGridStep = 4;         // Blocks between the heights a column blends.
constexpr usize kGridPoints = 5;     // Per side of a chunk, both edges included.
constexpr f64 kSlopeReach = 6.0;     // Blocks over which the planet's slope is measured.
constexpr f64 kGullyWidest = 90.0;   // Blocks across the widest gullies.
constexpr f64 kGullyStrength = 0.15; // Depth of a gully, as a part of its width.
constexpr int kGullyOctaves = 3;
constexpr f64 kRockSlope = 1.6;           // Steeper ground than this holds no soil.
constexpr f64 kSnowSlope = 3.4;           // Steeper ground than this holds no snow.
constexpr f64 kOverhangMost = 12.0;       // Blocks a rock face leans at most.
constexpr i64 kTreeHeadroom = 32;         // The tallest tree, above any ground.
constexpr i64 kFeatureCell = 19;          // Blocks per side of a boulder or fallen log cell.
constexpr i64 kFeatureReach = 7;          // How far those reach from their cell's spot.
constexpr usize kSharedColumns = 8192;    // For all threads, a power of two.
constexpr usize kCacheColumns = 256;      // Per thread, a power of two.
constexpr f64 kFarRiverCell = 4.0;        // Far cells wider than this leave rivers and lakes out.
constexpr f64 kDryGreenDamp = 0.8;        // The same on dry land, where water does less.
constexpr f64 kPalmWarmth = 16.0;         // Degrees: cooler coasts have no palms.
constexpr f64 kGreenDamp = 0.45;          // How damp ground must be to turn green by itself.
constexpr f64 kDryWatersidePlants = 0.16; // The same on dry land.
constexpr f64 kWatersidePlants = 0.72;    // Share of the wettest ground under plants.
constexpr i64 kBandHeight = 5;            // Blocks of one rock layer.
// Deepslate below this height; between it and kDeepslateMix it mixes with
// stone, more of it the lower, as in Minecraft.
constexpr i64 kDeepslateTop = 0;
constexpr i64 kDeepslateMix = 8;
// Lava lakes: every square region of kLavaRegion blocks has one lake level or
// none, so a lake's surface is flat. kLavaShare of regions (in hundredths)
// hold one, at kLavaHighest down to kLavaSteps steps of kLavaStep lower. Cave
// air up to kLavaDepth blocks below the level is lava, on kLavaFloor blocks of
// rock; caves go on below that, so the deep is not flooded (the world reaches
// a thousand blocks down, not Minecraft's sixty four). Within kLavaWall
// blocks of a region whose lake stands where this one has open air, cave air
// is rock, so lava never stands beside open air.
constexpr i64 kLavaRegion = 192;
constexpr u64 kLavaShare = 70;
constexpr i64 kLavaHighest = -40;
constexpr i64 kLavaStep = 8;
constexpr u64 kLavaSteps = 40;
constexpr i64 kLavaDepth = 16;
constexpr i64 kLavaFloor = 4;
constexpr i64 kLavaWall = 3;

constexpr u64 mix(u64 value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

constexpr u64 hash3(u64 seed, i64 x, i64 y, i64 z) noexcept {
    return mix(seed ^ mix(static_cast<u64>(x) ^ mix(static_cast<u64>(y) ^ mix(static_cast<u64>(z)))));
}

i64 floorDiv(i64 value, i64 divisor) noexcept {
    return value >= 0 ? value / divisor : ((value + 1) / divisor) - 1;
}

// A lake's surface as a height in blocks.
f64 lakeOf(f64 level) noexcept {
    return level > kNoWater ? level + static_cast<f64>(Terrain::kSeaLevel) : kNoWater;
}

// A smooth wave with a period of 1, 1 at whole numbers and -1 half way, and
// its slope. A stand in for a cosine: library cosines differ between machines,
// and worlds must not (see vxe/math/noise.hpp).
struct Wave {
    f64 value = 0.0;
    f64 slope = 0.0;
};
Wave wave(f64 phase) noexcept {
    const f64 t = phase - std::floor(phase);
    const f64 side = t < 0.5 ? 1.0 : -1.0;
    const f64 s = t < 0.5 ? 1.0 - (2.0 * t) : (2.0 * t) - 1.0; // 1 at whole numbers, 0 half way.
    return {.value = (2.0 * s * s * (3.0 - (2.0 * s))) - 1.0, .slope = -24.0 * s * (1.0 - s) * side};
}

// A 3D field on a grid of 4 x 8 x 4 blocks, 5 x 3 x 5 points for a chunk
// (Section 12.2): noise is costly per block, and rock and overhangs change
// slowly enough to blend.
using ChunkField = std::array<f64, usize{5} * 3 * 5>;

template <typename Noise>
void fillField(ChunkField& field, const vxe::BlockPos& origin, Noise&& noise) noexcept {
    for (usize y = 0; y < 3; ++y) {
        for (usize z = 0; z < 5; ++z) {
            for (usize x = 0; x < 5; ++x) {
                field[(y * 25) + (z * 5) + x] =
                    noise(static_cast<f64>(origin.x) + (static_cast<f64>(x) * 4.0),
                          static_cast<f64>(origin.y) + (static_cast<f64>(y) * 8.0),
                          static_cast<f64>(origin.z) + (static_cast<f64>(z) * 4.0));
            }
        }
    }
}

f64 fieldAt(const ChunkField& field, u32 x, u32 y, u32 z) noexcept {
    const usize gx = x / 4;
    const usize gy = y / 8;
    const usize gz = z / 4;
    const f64 tx = static_cast<f64>(x % 4) / 4.0;
    const f64 ty = static_cast<f64>(y % 8) / 8.0;
    const f64 tz = static_cast<f64>(z % 4) / 4.0;
    const auto layer = [&](usize at) {
        const f64* row = &field[(at * 25) + (gz * 5) + gx];
        const f64 near = row[0] + ((row[1] - row[0]) * tx);
        const f64 far = row[5] + ((row[6] - row[5]) * tx);
        return near + ((far - near) * tz);
    };
    const f64 below = layer(gy);
    return below + ((layer(gy + 1) - below) * ty);
}

// A 3D field on a grid of 4 blocks in every direction, 5 x 5 x 5 points for a
// chunk: caves need finer steps upwards than rock does, and still far fewer
// noise samples than one per block.
using CaveField = std::array<f64, usize{5} * 5 * 5>;

template <typename Noise>
void fillCaveField(CaveField& field, const vxe::BlockPos& origin, Noise&& noise) noexcept {
    for (usize y = 0; y < 5; ++y) {
        for (usize z = 0; z < 5; ++z) {
            for (usize x = 0; x < 5; ++x) {
                field[(y * 25) + (z * 5) + x] =
                    noise(static_cast<f64>(origin.x) + (static_cast<f64>(x) * 4.0),
                          static_cast<f64>(origin.y) + (static_cast<f64>(y) * 4.0),
                          static_cast<f64>(origin.z) + (static_cast<f64>(z) * 4.0));
            }
        }
    }
}

f64 caveFieldAt(const CaveField& field, u32 x, u32 y, u32 z) noexcept {
    const usize gx = x / 4;
    const usize gy = y / 4;
    const usize gz = z / 4;
    const f64 tx = static_cast<f64>(x % 4) / 4.0;
    const f64 ty = static_cast<f64>(y % 4) / 4.0;
    const f64 tz = static_cast<f64>(z % 4) / 4.0;
    const auto layer = [&](usize at) {
        const f64* row = &field[(at * 25) + (gz * 5) + gx];
        const f64 near = row[0] + ((row[1] - row[0]) * tx);
        const f64 far = row[5] + ((row[6] - row[5]) * tx);
        return near + ((far - near) * tz);
    };
    const f64 below = layer(gy);
    return below + ((layer(gy + 1) - below) * ty);
}

// A number from noise-like patches: above `1 - 2 * share` for about that share
// of the ground. Two scales, so patch edges are ragged instead of round blobs.
bool inPatch(u64 seed, f64 x, f64 z, f64 share) noexcept {
    const f64 shape = vxe::noise::gradient2(seed, x / 9.0, z / 9.0).value +
                      (vxe::noise::gradient2(seed + 1, x / 3.0, z / 3.0).value * 0.45);
    return shape > 0.62 - (share * 1.24);
}

// The part of a tree or other feature that falls inside one chunk.
class Stamp {
public:
    Stamp(const vxe::BlockPos& origin, std::span<BlockStateId, vxe::kChunkVolume> out) noexcept
        : m_origin(origin)
        , m_out(out) {}

    void put(i64 x, i64 y, i64 z, BlockStateId state, bool replaceSolid) const noexcept {
        const i64 lx = x - m_origin.x;
        const i64 ly = y - m_origin.y;
        const i64 lz = z - m_origin.z;
        if (lx < 0 || ly < 0 || lz < 0 || lx >= vxe::kChunkSize || ly >= vxe::kChunkSize ||
            lz >= vxe::kChunkSize) {
            return;
        }
        const vxe::u16 index =
            vxe::LocalPos{.x = static_cast<u8>(lx), .y = static_cast<u8>(ly), .z = static_cast<u8>(lz)}
                .index();
        if (replaceSolid || m_out[index] == vxe::voxel::kAirState) {
            m_out[index] = state;
        }
    }

private:
    vxe::BlockPos m_origin;
    std::span<BlockStateId, vxe::kChunkVolume> m_out;
};

// Builds one tree block by block.
class TreeStamp {
public:
    TreeStamp(const Stamp& stamp, const TreeBlocks& blocks, u64 seed) noexcept
        : m_stamp(stamp)
        , m_blocks(blocks)
        , m_seed(seed) {}

    void log(i64 x, i64 y, i64 z) const noexcept { m_stamp.put(x, y, z, m_blocks.log, true); }
    void logAlong(i64 x, i64 y, i64 z, bool alongX) const noexcept {
        m_stamp.put(x, y, z, alongX ? m_blocks.logX : m_blocks.logZ, true);
    }

    void leaf(i64 x, i64 y, i64 z) const noexcept {
        const bool mixed = m_blocks.mixedShare > 0.0F &&
                           unit(hash3(m_seed, x, y, z)) < static_cast<f64>(m_blocks.mixedShare);
        m_stamp.put(x, y, z, mixed ? m_blocks.mixedLeaves : m_blocks.leaves, false);
    }

    // A flat layer of leaves. Trimming takes away the corners, so that crowns
    // look round; ragged also bites random pieces from the rim.
    void disc(i64 cx, i64 y, i64 cz, i64 radius, bool ragged) const noexcept {
        for (i64 dz = -radius; dz <= radius; ++dz) {
            for (i64 dx = -radius; dx <= radius; ++dx) {
                const i64 distance = (dx * dx) + (dz * dz);
                const i64 limit = (radius * radius) + radius;
                if (distance > limit) {
                    continue;
                }
                if (ragged && distance > (radius * radius) - radius &&
                    unit(hash3(m_seed + 1, cx + dx, y, cz + dz)) < 0.35) {
                    continue;
                }
                leaf(cx + dx, y, cz + dz);
            }
        }
    }

    // A rounded lump of leaves, ragged at its edge.
    void blob(i64 cx, i64 cy, i64 cz, i64 radius, i64 tall) const noexcept {
        for (i64 dy = -tall; dy <= tall; ++dy) {
            const f64 up = static_cast<f64>(dy) / static_cast<f64>(tall);
            const auto reach = static_cast<f64>(radius) * std::sqrt(std::max(0.0, 1.0 - (up * up)));
            disc(cx, cy + dy, cz, static_cast<i64>(reach + 0.5), true);
        }
    }

private:
    const Stamp& m_stamp;
    const TreeBlocks& m_blocks;
    u64 m_seed;
};

void trunk(const TreeStamp& tree, i64 x, i64 ground, i64 z, i64 top, bool wide) noexcept {
    for (i64 y = ground + 1; y < top; ++y) {
        tree.log(x, y, z);
        if (wide) {
            tree.log(x + 1, y, z);
            tree.log(x, y, z + 1);
            tree.log(x + 1, y, z + 1);
        }
    }
}

// A branch from the trunk at height y to (x, y + rise, z), with a lump of
// leaves at its end.
void branch(const TreeStamp& tree, i64 fromX, i64 fromZ, i64 y, i64 dx, i64 dz, i64 rise,
            i64 radius) noexcept {
    const i64 steps = std::max(std::abs(dx), std::abs(dz));
    for (i64 step = 1; step <= steps; ++step) {
        tree.logAlong(fromX + ((dx * step) / steps), y + ((rise * step) / steps),
                      fromZ + ((dz * step) / steps), std::abs(dx) >= std::abs(dz));
    }
    tree.blob(fromX + dx, y + rise + 1, fromZ + dz, radius, std::max<i64>(radius - 1, 1));
}

void buildTree(const TreeStamp& builder, const Terrain::Tree& tree, i64 ground) noexcept {
    const i64 x = tree.x;
    const i64 z = tree.z;
    const i64 top = ground + tree.trunk;
    u64 random = tree.seed;
    const auto roll = [&](i64 count) {
        random = mix(random);
        return static_cast<i64>(random % static_cast<u64>(count));
    };
    switch (tree.kind) {
    case TreeKind::Oak:
    case TreeKind::Birch:
    case TreeKind::TallBirch:
    case TreeKind::Jungle:
        builder.disc(x, top - 2, z, 2, true);
        builder.disc(x, top - 1, z, 2, true);
        builder.disc(x, top, z, 1, false);
        builder.disc(x, top + 1, z, 1, true);
        builder.leaf(x, top + 1, z);
        trunk(builder, x, ground, z, top, false);
        break;
    case TreeKind::BigOak: {
        builder.blob(x, top, z, 4, 3);
        branch(builder, x, z, top - 4, 3 - roll(2), 1 - roll(3), 2, 3);
        branch(builder, x, z, top - 3, roll(3) - 1, -3 + roll(2), 2, 3);
        branch(builder, x, z, top - 3, -3 + roll(2), roll(3) - 1, 1, 3);
        trunk(builder, x, ground, z, top + 1, false);
        break;
    }
    case TreeKind::TallSpruce:
    case TreeKind::Spruce: {
        // Skirts of branches, wider further down, alternating with thin rings.
        // The wood stops below the tip, so that the peak is all leaves, and a
        // thick trunk thins out well before it.
        const i64 crownTop = ground + tree.height;
        for (i64 layer = 0; layer < tree.crownDepth; ++layer) {
            const i64 wide = std::min<i64>((layer + 2) / 3 + 1, tree.radius);
            builder.disc(x, crownTop - layer, z, layer == 0 ? 0 : (layer % 2 == 0 ? wide : wide - 1), false);
        }
        trunk(builder, x, ground, z, crownTop - 1, false);
        if (tree.kind == TreeKind::TallSpruce && tree.height > 23) {
            trunk(builder, x, ground, z, crownTop - 9, true);
        }
        break;
    }
    case TreeKind::Pine: {
        const i64 crownTop = ground + tree.height;
        builder.disc(x, crownTop, z, 0, false);
        builder.disc(x, crownTop - 1, z, 1, false);
        builder.disc(x, crownTop - 2, z, 2, true);
        builder.disc(x, crownTop - 3, z, 1, false);
        builder.disc(x, crownTop - 4, z, 2, true);
        trunk(builder, x, ground, z, crownTop, false);
        break;
    }
    case TreeKind::DarkOak:
    case TreeKind::PaleOak: {
        builder.disc(x, top - 1, z, 4, true);
        builder.disc(x + 1, top, z + 1, 5, true);
        builder.disc(x + 1, top + 1, z, 3, true);
        builder.disc(x, top + 2, z + 1, 2, true);
        trunk(builder, x, ground, z, top + 1, true);
        break;
    }
    case TreeKind::Acacia: {
        // The trunk leans out, then opens into a flat canopy.
        const i64 dx = tree.crownX - x;
        const i64 dz = tree.crownZ - z;
        trunk(builder, x, ground, z, top - 2, false);
        const i64 lean = std::max({std::abs(dx), std::abs(dz), i64{1}});
        for (i64 step = 0; step <= lean; ++step) {
            const i64 y = top - 2 + ((step * 2) / lean);
            builder.log(x + ((dx * step) / lean), y, z + ((dz * step) / lean));
        }
        builder.disc(tree.crownX, top + 1, tree.crownZ, 4, true);
        builder.disc(tree.crownX, top + 2, tree.crownZ, 2, true);
        // A second, lower canopy on the other side.
        builder.log(x - (dx / 2), top - 2, z - (dz / 2));
        builder.disc(x - (dx / 2), top - 1, z - (dz / 2), 2, true);
        break;
    }
    case TreeKind::GiantJungle: {
        const i64 crownTop = ground + tree.height;
        builder.blob(x, crownTop - 2, z, 5, 2);
        builder.blob(x + 1, crownTop - 4, z + 1, 6, 2);
        for (i64 y = top - 7; y < top - 2; y += 4) {
            branch(builder, x, z, y, 2 + roll(2), roll(5) - 2, 1, 2);
            branch(builder, x, z, y + 2, -2 - roll(2), roll(5) - 2, 1, 2);
        }
        trunk(builder, x, ground, z, top, true);
        break;
    }
    case TreeKind::Azalea:
        builder.blob(x, top, z, 3, 2);
        trunk(builder, x, ground, z, top, false);
        break;
    case TreeKind::Bush:
        builder.log(x, ground + 1, z);
        builder.blob(x, ground + 2, z, 2, 1);
        break;
    case TreeKind::Palm: {
        // A slim trunk that curves away from straight, and long fronds that
        // arch out from its head and hang down at their tips.
        const i64 dx = tree.crownX - x;
        const i64 dz = tree.crownZ - z;
        i64 lastX = x;
        i64 lastZ = z;
        for (i64 y = 1; y <= tree.trunk; ++y) {
            const i64 px = x + ((dx * y * y) / (tree.trunk * tree.trunk));
            const i64 pz = z + ((dz * y * y) / (tree.trunk * tree.trunk));
            builder.log(lastX, ground + y, lastZ);
            builder.log(px, ground + y, pz);
            lastX = px;
            lastZ = pz;
        }
        const i64 head = ground + tree.trunk;
        builder.disc(lastX, head, lastZ, 1, false);
        builder.leaf(lastX, head + 1, lastZ);
        static constexpr std::array<std::array<i64, 2>, 8> kWays = {
            {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};
        for (const auto& way : kWays) {
            const i64 reach = 4 + roll(2) - (way[0] != 0 && way[1] != 0 ? 1 : 0);
            for (i64 step = 1; step <= reach; ++step) {
                const i64 drop = step < 3 ? 0 : (step - 2);
                const i64 fx = lastX + (way[0] * step);
                const i64 fz = lastZ + (way[1] * step);
                builder.leaf(fx, head + 1 - drop, fz);
                if (step == reach) {
                    builder.leaf(fx, head - drop, fz);
                }
            }
        }
        break;
    }
    case TreeKind::Olive: {
        // Short and gnarled: a thick stump that splits low into crooked limbs,
        // each with a small flat crown.
        trunk(builder, x, ground, z, ground + tree.trunk, true);
        const i64 fork = ground + tree.trunk;
        branch(builder, x + 1, z, fork, 3 - roll(2), roll(3) - 1, 2, 2);
        branch(builder, x, z, fork, -3 + roll(2), roll(3) - 1, 1 + roll(2), 2);
        branch(builder, x, z + 1, fork, roll(3) - 1, 3 - roll(2), 2, 2);
        branch(builder, x, z, fork, roll(3) - 1, -3 + roll(2), 1 + roll(2), 2);
        builder.blob(x, ground + tree.height - 1, z, 2, 1);
        builder.log(x, fork, z);
        builder.log(x, fork + 1, z);
        break;
    }
    case TreeKind::Maple:
    case TreeKind::GoldMaple: {
        // A full round crown, taller than wide, with lobes on its flanks.
        builder.blob(x, top, z, 4, 3);
        builder.blob(x, top + 3, z, 3, 2);
        branch(builder, x, z, top - 3, 2 + roll(2), roll(3) - 1, 2, 2);
        branch(builder, x, z, top - 3, -2 - roll(2), roll(3) - 1, 2, 2);
        trunk(builder, x, ground, z, top + 2, false);
        break;
    }
    case TreeKind::Hemlock: {
        // Slimmer than a spruce and softer: short, ragged boughs whose tips
        // hang down, and a leader that nods over at the very top.
        const i64 crownTop = ground + tree.height;
        builder.leaf(x, crownTop, z);
        builder.leaf(x, crownTop - 1, z);
        builder.leaf(x + 1, crownTop, z);
        for (i64 layer = 2; layer < tree.crownDepth; ++layer) {
            const i64 wide = std::min<i64>((layer / 4) + 1, tree.radius);
            const i64 y = crownTop - layer;
            builder.disc(x, y, z, layer % 3 == 2 ? wide : std::max<i64>(wide - 1, 1), true);
            if (layer % 3 == 2) {
                builder.leaf(x + wide + 1, y - 1, z);
                builder.leaf(x - wide - 1, y - 1, z);
                builder.leaf(x, y - 1, z + wide + 1);
                builder.leaf(x, y - 1, z - wide - 1);
            }
        }
        trunk(builder, x, ground, z, crownTop - 1, false);
        break;
    }
    case TreeKind::Poplar: {
        // A tall narrow column, widest low in its middle, tapering to a point.
        const i64 crownTop = ground + tree.height;
        const i64 first = crownTop - tree.crownDepth;
        for (i64 y = first; y <= crownTop; ++y) {
            const i64 depth = y - first;
            const i64 left = crownTop - y;
            builder.disc(x, y, z, left < 2 ? 0 : (depth > 1 && left > 4 ? 2 : 1), left > 2);
        }
        trunk(builder, x, ground, z, crownTop - 2, false);
        break;
    }
    case TreeKind::WeepingBirch: {
        // A rounded head with long strands of leaves hanging from its rim.
        builder.blob(x, top, z, 3, 2);
        static constexpr std::array<std::array<i64, 2>, 12> kStrands = {{{4, 0},
                                                                         {-4, 0},
                                                                         {0, 4},
                                                                         {0, -4},
                                                                         {3, 2},
                                                                         {3, -2},
                                                                         {-3, 2},
                                                                         {-3, -2},
                                                                         {2, 3},
                                                                         {2, -3},
                                                                         {-2, 3},
                                                                         {-2, -3}}};
        for (const auto& strand : kStrands) {
            const i64 length = 3 + roll(4);
            for (i64 down = 0; down < length; ++down) {
                builder.leaf(x + strand[0], top - down, z + strand[1]);
            }
        }
        trunk(builder, x, ground, z, top + 1, false);
        break;
    }
    case TreeKind::ForkedBirch: {
        // Splits into two or three leaning stems, each with its own head.
        const i64 fork = top - 4;
        builder.blob(x, top, z, 2, 2);
        const i64 sideX = 2 + roll(2);
        const i64 sideZ = roll(5) - 2;
        branch(builder, x, z, fork, sideX, sideZ, 3, 2);
        branch(builder, x, z, fork + 1, -sideX, -sideZ, 3 - roll(2), 2);
        trunk(builder, x, ground, z, top + 1, false);
        break;
    }
    }
}

template <typename Choice>
const Choice* pickWeighted(std::span<const Choice> choices, f64 roll) noexcept {
    u32 total = 0;
    for (const Choice& choice : choices) {
        total += choice.weight;
    }
    auto left = static_cast<u32>(roll * static_cast<f64>(total));
    for (const Choice& choice : choices) {
        if (left < choice.weight) {
            return &choice;
        }
        left -= choice.weight;
    }
    return choices.empty() ? nullptr : &choices.back();
}

} // namespace

// The heights of one chunk column, indexed by z * 16 + x.
struct Terrain::Column {
    u64 identity = 0;
    i32 chunkX = 0;
    i32 chunkZ = 0;
    bool valid = false;
    i32 lowest = 0;
    i32 highest = 0;
    f32 leanMost = 0.0F; // The highest of `lean`.
    std::array<i32, vxe::kChunkArea> heights{};
    std::array<f32, vxe::kChunkArea> lean{}; // Blocks the rock face leans here.
    std::array<BlockStateId, vxe::kChunkArea> top{};
    std::array<Biome, vxe::kChunkArea> biome{};
    std::array<bool, vxe::kChunkArea> soil{}; // Plants and trees can root here.
    std::array<f32, vxe::kChunkArea> damp{};  // How much water feeds the ground here.
    std::array<i32, vxe::kChunkArea> water{}; // The water surface over this column.
};

Terrain::Terrain(const TerrainBlocks& blocks, u64 seed, const PlanetPreset& preset, bool rivers) noexcept
    : m_blocks(blocks)
    , m_seed(seed)
    , m_identity(mix(seed + (rivers ? 1U : 0U)) ^ vxe::hashBytes(std::as_bytes(std::span(&preset, 1))))
    , m_planet(seed, preset)
    , m_climate(seed, m_planet)
    , m_rivers(m_planet)
    , m_shared(std::make_shared<SharedColumns>())
    , m_riversOn(rivers) {}

void Terrain::gullies(f64 x, f64 z, f64 shortest, f64 strength, Ground& out, f64& slopeX,
                      f64& slopeZ) const noexcept {
    f64 widest = kGullyWidest;
    u64 seed = m_seed + 31;
    for (int octave = 0; octave < kGullyOctaves && widest >= shortest; ++octave) {
        const f64 steepness = std::sqrt((slopeX * slopeX) + (slopeZ * slopeZ));
        if (steepness < 1.0e-6) {
            return;
        }
        // Across the slope: moving this way goes neither up nor down, so
        // stripes that change along it run downhill.
        const f64 acrossX = -slopeZ / steepness;
        const f64 acrossZ = slopeX / steepness;
        const f64 px = x / widest;
        const f64 pz = z / widest;
        const auto cellX = static_cast<i64>(std::floor(px));
        const auto cellZ = static_cast<i64>(std::floor(pz));
        // Stripes come from nearby random spots, each laying its own stripes
        // through itself, blended by distance. One global set of stripes
        // would bend with the slope into rings.
        f64 weights = 0.0;
        f64 value = 0.0;
        f64 change = 0.0;
        for (i64 dz = -1; dz <= 1; ++dz) {
            for (i64 dx = -1; dx <= 1; ++dx) {
                const u64 h = vxe::noise::hash2(seed, cellX + dx, cellZ + dz);
                const f64 gapX = px - (static_cast<f64>(cellX + dx) + unit(h));
                const f64 gapZ = pz - (static_cast<f64>(cellZ + dz) + unit(mix(h)));
                const f64 near = 1.0 - (((gapX * gapX) + (gapZ * gapZ)) / 2.25);
                if (near <= 0.0) {
                    continue;
                }
                const f64 weight = near * near * near;
                const Wave stripes = wave((gapX * acrossX) + (gapZ * acrossZ));
                weights += weight;
                value += stripes.value * weight;
                change += stripes.slope * weight;
            }
        }
        if (weights <= 0.0) {
            return;
        }
        // More channel than ridge: erosion takes ground away.
        const f64 depth = widest * kGullyStrength * strength;
        out.height += ((value / weights) - 0.3) * depth;
        // The bend of this octave, for the next to follow.
        const f64 bend = (change / weights) * depth / widest;
        slopeX += bend * acrossX;
        slopeZ += bend * acrossZ;
        widest *= 0.5;
        seed = mix(seed);
    }
}

Ground Terrain::ground(f64 x, f64 z, f64 shortest) const noexcept {
    const PlanetSample here = m_planet.sample(x, z);
    f64 slopeX = (m_planet.sample(x + kSlopeReach, z).elevation - here.elevation) / kSlopeReach;
    f64 slopeZ = (m_planet.sample(x, z + kSlopeReach).elevation - here.elevation) / kSlopeReach;
    Ground out{.height = here.elevation + static_cast<f64>(kSeaLevel),
               .slope = std::sqrt((slopeX * slopeX) + (slopeZ * slopeZ)),
               .mountain = here.mountain,
               .rift = here.rift,
               .rough = here.rough,
               .continent = here.continent};
    out.water = m_riversOn && shortest <= kFarRiverCell * 2.0 ? lakeOf(m_rivers.lakeAt(x, z)) : kNoWater;
    // Water carves what stands above the sea and is steep enough to run.
    const f64 strength = smooth(0.06, 0.55, out.slope) * smooth(2.0, 50.0, here.elevation);
    if (strength > 0.0) {
        gullies(x, z, shortest, strength, out, slopeX, slopeZ);
        out.slope = std::sqrt((slopeX * slopeX) + (slopeZ * slopeZ));
    }
    return out;
}

Ground Terrain::farGround(f64 x, f64 z, f64 cell) const noexcept {
    if (cell >= kGullyWidest) {
        // Wider than any gully, and too wide for a slope to mean much.
        const PlanetSample here = m_planet.sample(x, z);
        return {.height = here.elevation + static_cast<f64>(kSeaLevel),
                .mountain = here.mountain,
                .rift = here.rift,
                .rough = here.rough,
                .continent = here.continent,
                .water = kNoWater};
    }
    return ground(x, z, cell * 2.0);
}

void Terrain::carveInto(std::span<const RiverNetwork::Stretch> nearby, f64 x, f64 z, Ground& at) noexcept {
    if (nearby.empty()) {
        return;
    }
    const auto sea = static_cast<f64>(kSeaLevel);
    const RiverNetwork::Carve carved = RiverNetwork::carve(nearby, x, z, at.height - sea);
    at.height = carved.ground + sea;
    if (carved.water > kNoWater) {
        at.water = std::max(at.water, carved.water + sea);
    }
    at.damp = std::max(at.damp, carved.damp);
    at.shore = carved.shore && at.height >= carved.ground;
}

BlockStateId Terrain::topOf(f64 x, f64 z, const Ground& at, Biome biome) const noexcept {
    const BiomeLook& look = m_blocks.biomes[static_cast<usize>(biome)];
    const f64 surface = std::max(at.water, static_cast<f64>(kSeaLevel));
    const bool underWater = at.height < surface;
    // Rivers and lakes have soft banks of the biome's floor.
    if (at.water > kNoWater && at.height < at.water + 0.5) {
        return look.floor;
    }
    if (!underWater) {
        const bool snowy = look.top == m_blocks.biomes[static_cast<usize>(Biome::SnowyPeaks)].top;
        if (at.slope > (snowy ? kSnowSlope : kRockSlope) && !look.banded) {
            return m_blocks.stone;
        }
    }
    // Water brings green, even into dry land: banks grow grass where the biome
    // around them is sand or bare rock. Snow stays snow.
    const BlockStateId snow = m_blocks.biomes[static_cast<usize>(Biome::SnowyPeaks)].top;
    if (!underWater && look.top != snow && look.top != m_blocks.soils[6]) {
        // On dry land only the wettest ground turns, and only in pieces.
        const bool green =
            look.dry ? at.damp > kDryGreenDamp && inPatch(m_seed + 59, x, z, 0.55) : at.damp > kGreenDamp;
        // Sand and gravel of the river bed show at the water's edge.
        if (at.shore && inPatch(m_seed + 53, x, z, 0.5)) {
            return look.floor;
        }
        if (green) {
            return m_blocks.soils[0];
        }
    }
    if (look.patchShare > 0.0F && inPatch(m_seed + 47, x, z, static_cast<f64>(look.patchShare))) {
        return look.patch;
    }
    if (look.patch2Share > 0.0F && inPatch(m_seed + 49, x, z, static_cast<f64>(look.patch2Share))) {
        return look.patch2;
    }
    return underWater ? look.floor : look.top;
}

Terrain::FarCell Terrain::farCell(f64 x, f64 z, f64 cell, const Climate::Field& field) const noexcept {
    Ground at = farGround(x, z, cell);
    // Rivers show as far as a cell can hold them.
    if (m_riversOn && cell <= kFarRiverCell) {
        thread_local std::vector<RiverNetwork::Stretch> nearby;
        nearby.clear();
        m_rivers.gather(x - cell, z - cell, x + cell, z + cell, nearby);
        carveInto(nearby, x, z, at);
    }
    const f64 above = at.height - static_cast<f64>(kSeaLevel);
    const ClimateSample weather = Climate::fromField(field, above, m_climate.coolPerBlock());
    const Biome biome = Climate::biome({.elevation = above,
                                        .continent = at.continent,
                                        .mountain = at.mountain,
                                        .rift = at.rift,
                                        .rough = at.rough},
                                       weather);
    return {.height = static_cast<i64>(std::floor(at.height)),
            .biome = biome,
            .top = topOf(x, z, at, biome),
            .water = at.water > kNoWater ? static_cast<i64>(at.water) : kSeaLevel};
}

f64 Terrain::treeCover(Biome biome) const noexcept {
    // Crowns cover about 24 blocks each, in cells of 25.
    return std::min(1.0, static_cast<f64>(m_blocks.biomes[static_cast<usize>(biome)].trees) * 1.3);
}

// Held for the few hundred nanoseconds of a copy, so spinning beats sleeping.
class SpinLock {
public:
    explicit SpinLock(std::atomic_flag& flag) noexcept
        : m_flag(flag) {
        while (m_flag.test_and_set(std::memory_order_acquire)) {
            while (m_flag.test(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
        }
    }
    ~SpinLock() { m_flag.clear(std::memory_order_release); }
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

private:
    std::atomic_flag& m_flag;
};

struct Terrain::SharedColumns {
    struct Slot {
        std::atomic_flag busy;
        std::unique_ptr<Column> column; // Made when first used: most slots of a small view stay empty.
    };
    std::vector<Slot> slots = std::vector<Slot>(kSharedColumns);
};

void Terrain::buildColumn(i32 chunkX, i32 chunkZ, Column& column) const noexcept {
    const vxe::BlockPos origin = vxe::chunkOrigin({.x = chunkX, .y = 0, .z = chunkZ});
    std::array<Ground, kGridPoints * kGridPoints> grid;
    for (usize gz = 0; gz < kGridPoints; ++gz) {
        for (usize gx = 0; gx < kGridPoints; ++gx) {
            grid[(gz * kGridPoints) + gx] =
                ground(static_cast<f64>(origin.x + (static_cast<i64>(gx) * kGridStep)),
                       static_cast<f64>(origin.z + (static_cast<i64>(gz) * kGridStep)), 1.0);
        }
    }
    const Climate::Cell weatherCell = m_climate.cell(static_cast<f64>(origin.x), static_cast<f64>(origin.z));
    thread_local std::vector<RiverNetwork::Stretch> nearby;
    nearby.clear();
    if (m_riversOn) {
        m_rivers.gather(static_cast<f64>(origin.x), static_cast<f64>(origin.z),
                        static_cast<f64>(origin.x + vxe::kChunkSize),
                        static_cast<f64>(origin.z + vxe::kChunkSize), nearby);
    }
    column.identity = m_identity;
    column.chunkX = chunkX;
    column.chunkZ = chunkZ;
    column.valid = true;
    column.lowest = 1 << 30;
    column.highest = -(1 << 30);
    column.leanMost = 0.0F;
    for (i64 z = 0; z < vxe::kChunkSize; ++z) {
        for (i64 x = 0; x < vxe::kChunkSize; ++x) {
            const auto gx = static_cast<usize>(x / kGridStep);
            const auto gz = static_cast<usize>(z / kGridStep);
            const f64 tx = static_cast<f64>(x % kGridStep) / static_cast<f64>(kGridStep);
            const f64 tz = static_cast<f64>(z % kGridStep) / static_cast<f64>(kGridStep);
            const auto blend = [&](f64 Ground::* part) {
                const Ground* row = &grid[(gz * kGridPoints) + gx];
                const f64 near = row[0].*part + ((row[1].*part - row[0].*part) * tx);
                const f64 far =
                    row[kGridPoints].*part + ((row[kGridPoints + 1].*part - row[kGridPoints].*part) * tx);
                return near + ((far - near) * tz);
            };
            const auto wx = static_cast<f64>(origin.x + x);
            const auto wz = static_cast<f64>(origin.z + z);
            Ground here{.height = blend(&Ground::height),
                        .slope = blend(&Ground::slope),
                        .mountain = blend(&Ground::mountain),
                        .rift = blend(&Ground::rift),
                        .rough = blend(&Ground::rough),
                        .continent = blend(&Ground::continent)};
            // A surface does not blend: water stands at the level of the
            // nearest corner that has any.
            const Ground* corners = &grid[(gz * kGridPoints) + gx];
            here.water = std::max({corners[0].water, corners[1].water, corners[kGridPoints].water,
                                   corners[kGridPoints + 1].water});
            // The grid is 4 blocks wide; this gives the ground its last grain.
            f64 above = here.height - static_cast<f64>(kSeaLevel);
            here.height +=
                vxe::noise::gradient2(m_seed + 37, wx / 9.0, wz / 9.0).value * smooth(1.0, 8.0, above) * 1.6;
            // Per block, not on the grid: a stream can be one block wide.
            carveInto(nearby, wx, wz, here);
            // A lake feeds its shores the way a river feeds its banks.
            if (here.water > kNoWater) {
                here.damp = std::max(here.damp, 1.0 - smooth(0.0, 6.0, here.height - here.water));
            }
            above = here.height - static_cast<f64>(kSeaLevel);
            ClimateSample weather = Climate::sample(weatherCell, wx, wz, above);
            // Biome borders fray a little, so they never run as smooth curves.
            const f64 fray = vxe::noise::gradient2(m_seed + 53, wx / 11.0, wz / 11.0).value;
            weather.temperature += fray * 1.6;
            weather.rainfall += fray * 0.045;
            const Biome biome = Climate::biome({.elevation = above,
                                                .continent = here.continent,
                                                .mountain = here.mountain,
                                                .rift = here.rift,
                                                .rough = here.rough},
                                               weather);
            const auto height = static_cast<i32>(std::floor(here.height));
            const auto index = static_cast<usize>((z * vxe::kChunkSize) + x);
            column.heights[index] = height;
            column.biome[index] = biome;
            column.top[index] = topOf(wx, wz, here, biome);
            const i32 surface =
                here.water > kNoWater ? static_cast<i32>(here.water) : static_cast<i32>(kSeaLevel);
            column.water[index] = surface;
            column.soil[index] = height >= surface && m_blocks.isSoil(column.top[index]);
            column.highest = std::max(column.highest, surface);
            const auto lean =
                static_cast<f32>(kOverhangMost * smooth(1.8, 3.4, here.slope) * smooth(30.0, 140.0, above));
            column.lean[index] = lean;
            column.damp[index] = static_cast<f32>(here.damp);
            column.leanMost = std::max(column.leanMost, lean);
            column.lowest = std::min(column.lowest, height);
            column.highest = std::max(column.highest, height);
        }
    }
}

const Terrain::Column& Terrain::column(i32 chunkX, i32 chunkZ) const {
    // A plain direct mapped table: no locks, because each thread has its own,
    // and no eviction logic, because a wrong guess only costs a rebuild.
    thread_local std::vector<Column> cache(kCacheColumns);
    const u64 h =
        mix(m_identity ^ ((static_cast<u64>(static_cast<u32>(chunkX)) << 32U) | static_cast<u32>(chunkZ)));
    Column& entry = cache[h & (kCacheColumns - 1)];
    const auto holds = [&](const Column& column) {
        return column.valid && column.identity == m_identity && column.chunkX == chunkX &&
               column.chunkZ == chunkZ;
    };
    if (holds(entry)) {
        return entry;
    }
    // A long view holds far more columns than a thread keeps, and the chunks of
    // one column reach different workers, so without this each of them builds
    // it again. Columns are copied out to keep the reference this returns away
    // from other threads; a copy costs a thousandth of a build.
    SharedColumns::Slot& slot = m_shared->slots[(h >> 16U) & (kSharedColumns - 1)];
    {
        const SpinLock lock(slot.busy);
        if (slot.column != nullptr && holds(*slot.column)) {
            entry = *slot.column;
            return entry;
        }
    }
    buildColumn(chunkX, chunkZ, entry);
    const SpinLock lock(slot.busy);
    if (slot.column == nullptr) {
        slot.column = std::make_unique<Column>();
    }
    *slot.column = entry;
    return entry;
}

const Terrain::Column& Terrain::columnAt(i64 x, i64 z) const {
    return column(static_cast<i32>(floorDiv(x, vxe::kChunkSize)),
                  static_cast<i32>(floorDiv(z, vxe::kChunkSize)));
}

vxe::i64 Terrain::surfaceHeight(i64 x, i64 z) const {
    return columnAt(x, z).heights[static_cast<usize>(((z & 15) * vxe::kChunkSize) + (x & 15))];
}

Biome Terrain::biomeAt(i64 x, i64 z) const {
    return columnAt(x, z).biome[static_cast<usize>(((z & 15) * vxe::kChunkSize) + (x & 15))];
}

i64 Terrain::waterLevel(i64 x, i64 z) const {
    return columnAt(x, z).water[static_cast<usize>(((z & 15) * vxe::kChunkSize) + (x & 15))];
}

BlockStateId Terrain::topBlock(i64 x, i64 z) const {
    return columnAt(x, z).top[static_cast<usize>(((z & 15) * vxe::kChunkSize) + (x & 15))];
}

vxe::BlockPos Terrain::spawnColumn() const {
    // Rings around the origin, each 64 blocks further out: the origin can lie
    // in an ocean, so this reaches about 15 km.
    constexpr i64 kRingStep = 64;
    constexpr i64 kRings = 240;
    for (i64 ring = 0; ring < kRings; ++ring) {
        const i64 reach = ring * kRingStep;
        const i64 count = ring == 0 ? 1 : ring * 8;
        for (i64 i = 0; i < count; ++i) {
            // The square ring, side by side.
            const i64 side = ring == 0 ? 0 : i / (ring * 2);
            const i64 along = ring == 0 ? 0 : ((i % (ring * 2)) - ring) * kRingStep;
            const std::array<std::pair<i64, i64>, 4> corners = {
                {{along, -reach}, {reach, along}, {-along, reach}, {-reach, -along}}};
            const auto [x, z] = corners.at(static_cast<usize>(side));
            if (m_planet.landMass(static_cast<f64>(x), static_cast<f64>(z)) < 0.03) {
                continue; // Open sea.
            }
            const Ground at = ground(static_cast<f64>(x), static_cast<f64>(z), kGullyWidest);
            const f64 above = at.height - static_cast<f64>(kSeaLevel);
            if (above > 6.0 && above < 70.0 && at.slope < 0.3) {
                return {.x = x, .y = static_cast<i64>(std::floor(at.height)), .z = z};
            }
        }
    }
    return {.x = 0, .y = kSeaLevel + 8, .z = 0};
}

Ground Terrain::roughGround(f64 x, f64 z) const noexcept {
    return farGround(x, z, kGullyWidest);
}

Biome Terrain::roughBiome(f64 x, f64 z) const noexcept {
    const Ground at = roughGround(x, z);
    const f64 above = at.height - static_cast<f64>(kSeaLevel);
    return Climate::biome({.elevation = above,
                           .continent = at.continent,
                           .mountain = at.mountain,
                           .rift = at.rift,
                           .rough = at.rough},
                          m_climate.sample(x, z, above));
}

std::optional<vxe::BlockPos> Terrain::findBiome(Biome biome, i64 fromX, i64 fromZ) const {
    constexpr i64 kRingStep = 48;
    constexpr i64 kRings = 250;
    for (i64 ring = 0; ring < kRings; ++ring) {
        const i64 reach = ring * kRingStep;
        const i64 count = ring == 0 ? 1 : ring * 8;
        for (i64 i = 0; i < count; i += 1 + (ring / 40)) {
            const i64 side = ring == 0 ? 0 : i / (ring * 2);
            const i64 along = ring == 0 ? 0 : ((i % (ring * 2)) - ring) * kRingStep;
            const std::array<std::pair<i64, i64>, 4> corners = {
                {{along, -reach}, {reach, along}, {-along, reach}, {-reach, -along}}};
            const auto [dx, dz] = corners.at(static_cast<usize>(side));
            const i64 x = fromX + dx;
            const i64 z = fromZ + dz;
            const Ground at = ground(static_cast<f64>(x), static_cast<f64>(z), kGullyWidest);
            const f64 above = at.height - static_cast<f64>(kSeaLevel);
            const Biome here =
                Climate::biome({.elevation = above,
                                .continent = at.continent,
                                .mountain = at.mountain,
                                .rift = at.rift,
                                .rough = at.rough},
                               m_climate.sample(static_cast<f64>(x), static_cast<f64>(z), above));
            if (here == biome && biomeAt(x, z) == biome) {
                return vxe::BlockPos{.x = x, .y = surfaceHeight(x, z), .z = z};
            }
        }
    }
    return std::nullopt;
}

BlockStateId Terrain::stoneAt(i64 x, i64 y, i64 z, f64 a, f64 b) const noexcept {
    // Rock kinds in large blobs from two slow fields; ores come in veins
    // afterwards (placeOres).
    // The deep: deepslate, with tuff and basalt in it, and none of the
    // lighter rocks.
    const bool deep = y < kDeepslateTop ||
                      (y < kDeepslateMix &&
                       static_cast<i64>(hash3(m_seed + 96, x, y, z) % static_cast<u64>(kDeepslateMix)) >= y);
    if (deep) {
        if (b > 0.33) {
            return m_blocks.tuff;
        }
        return b < -0.36 ? m_blocks.basalt : m_blocks.deepslate;
    }
    if (a > 0.3) {
        return m_blocks.granite;
    }
    if (a < -0.3) {
        return m_blocks.andesite;
    }
    if (b > 0.33) {
        return m_blocks.tuff;
    }
    if (b < -0.36) {
        if (y > kSeaLevel + 60) {
            return m_blocks.calcite;
        }
        return y < kSeaLevel - 50 ? m_blocks.basalt : m_blocks.gravel;
    }
    return m_blocks.stone;
}

BlockStateId Terrain::bandAt(i64 x, i64 y, i64 z, bool banded) const noexcept {
    if (banded) {
        // Layers of rock, tilted and wavy, like the walls of a canyon.
        const f64 wobble =
            vxe::noise::gradient2(m_seed + 59, static_cast<f64>(x) / 40.0, static_cast<f64>(z) / 40.0).value *
            7.0;
        const i64 layer = floorDiv(y + static_cast<i64>(std::floor(wobble)), kBandHeight);
        const std::array<BlockStateId, 8> kinds = {
            m_blocks.redSandstone, m_blocks.sandstone, m_blocks.redSandstone, m_blocks.packedMud,
            m_blocks.redSandstone, m_blocks.granite,   m_blocks.sandstone,    m_blocks.redSandstone};
        return kinds.at(mix(m_seed + 61 + static_cast<u64>(layer)) % 8U);
    }
    // Mountain sides: rock kinds in random brushes of two sizes. Mostly stone,
    // so that the others read as patches, not as a pattern.
    const auto wx = static_cast<f64>(x);
    const auto wy = static_cast<f64>(y);
    const auto wz = static_cast<f64>(z);
    const f64 brush = vxe::noise::gradient3(m_seed + 59, wx / 11.0, wy / 9.0, wz / 11.0) +
                      (vxe::noise::gradient3(m_seed + 60, wx / 4.0, wy / 4.0, wz / 4.0) * 0.4);
    if (brush > 0.42) {
        return m_blocks.andesite;
    }
    if (brush < -0.42) {
        return m_blocks.tuff;
    }
    const f64 vein = vxe::noise::gradient3(m_seed + 62, wx / 17.0, wy / 13.0, wz / 17.0);
    if (vein > 0.44) {
        return m_blocks.granite;
    }
    return vein < -0.46 ? m_blocks.calcite : m_blocks.stone;
}

void Terrain::generate(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const Column& at = column(chunk.x, chunk.z);
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const auto leanReach = static_cast<i64>(std::ceil(at.leanMost));
    // Chunks wholly above ground and water are air: skip the work (Section 12.2).
    if (origin.y > std::max<i64>(at.highest + leanReach, kSeaLevel) + kTreeHeadroom) {
        std::ranges::fill(out, vxe::voxel::kAirState);
        return;
    }
    const bool leaning = leanReach > 0 && origin.y <= at.highest + leanReach &&
                         origin.y + vxe::kChunkSize > at.lowest - leanReach;
    thread_local ChunkField density;
    thread_local ChunkField rockA;
    thread_local ChunkField rockB;
    if (leaning) {
        const vxe::noise::Fractal shape{.octaves = 3, .frequency = 1.0 / 26.0};
        fillField(density, origin,
                  [&](f64 x, f64 y, f64 z) { return vxe::noise::fbm3(m_seed + 43, x, y, z, shape) * 1.8; });
    }
    const bool anyStone = origin.y <= at.highest + leanReach;
    // Caves (G3.4): long round tunnels where two noise fields both pass zero,
    // and wide caverns deep down where a slow field runs high.
    thread_local CaveField tunnelA;
    thread_local CaveField tunnelB;
    thread_local CaveField cavern;
    if (anyStone) {
        fillCaveField(tunnelA, origin, [&](f64 x, f64 y, f64 z) {
            return vxe::noise::gradient3(m_seed + 71, x / 52.0, y / 30.0, z / 52.0);
        });
        fillCaveField(tunnelB, origin, [&](f64 x, f64 y, f64 z) {
            return vxe::noise::gradient3(m_seed + 72, x / 52.0, y / 30.0, z / 52.0);
        });
        fillCaveField(cavern, origin, [&](f64 x, f64 y, f64 z) {
            return vxe::noise::gradient3(m_seed + 74, x / 96.0, y / 52.0, z / 96.0) +
                   (vxe::noise::gradient3(m_seed + 75, x / 37.0, y / 25.0, z / 37.0) * 0.45);
        });
    }
    if (anyStone) {
        fillField(rockA, origin, [&](f64 x, f64 y, f64 z) {
            return vxe::noise::gradient3(m_seed + 5, x / 34.0, y / 22.0, z / 34.0);
        });
        fillField(rockB, origin, [&](f64 x, f64 y, f64 z) {
            return vxe::noise::gradient3(m_seed + 6, x / 41.0, y / 26.0, z / 41.0);
        });
    }
    // Lava levels only matter below the highest a lake can stand.
    std::array<LavaColumn, static_cast<usize>(vxe::kChunkSize * vxe::kChunkSize)> lavaColumns{};
    lavaColumns.fill(
        LavaColumn{.level = kFloorY, .bottom = kFloorY, .floor = kFloorY, .wallLow = 0, .wallHigh = 0});
    if (anyStone && origin.y < kLavaHighest) {
        for (usize i = 0; i < lavaColumns.size(); ++i) {
            lavaColumns.at(i) = lavaAt(origin.x + static_cast<i64>(i % vxe::kChunkSize),
                                       origin.z + static_cast<i64>(i / vxe::kChunkSize));
        }
    }
    for (u32 index = 0; index < static_cast<u32>(vxe::kChunkVolume); ++index) {
        const vxe::LocalPos local = vxe::LocalPos::fromIndex(static_cast<vxe::u16>(index));
        const usize columnIndex = (usize{local.z} * vxe::kChunkSize) + local.x;
        const i64 x = origin.x + local.x;
        const i64 y = origin.y + local.y;
        const i64 z = origin.z + local.z;
        if (y <= kFloorY) {
            out[index] = m_blocks.floor;
            continue;
        }
        f64 top = at.heights[columnIndex];
        if (leaning && at.lean[columnIndex] > 0.0F) {
            top -= static_cast<f64>(at.lean[columnIndex]) * fieldAt(density, local.x, local.y, local.z);
        }
        const auto height = static_cast<f64>(y);
        if (height > top) {
            out[index] = y <= at.water[columnIndex] ? m_blocks.water : vxe::voxel::kAirState;
            continue;
        }
        const f64 depth = top - height;
        if (isCave(at, columnIndex, x, y, z, depth, caveFieldAt(tunnelA, local.x, local.y, local.z),
                   caveFieldAt(tunnelB, local.x, local.y, local.z),
                   caveFieldAt(cavern, local.x, local.y, local.z))) {
            const LavaColumn& lava = lavaColumns.at(columnIndex);
            const bool walled = y >= lava.wallLow && y < lava.wallHigh;
            if (!walled && y < lava.level && y >= lava.bottom) {
                out[index] = m_blocks.lava;
                continue;
            }
            if (!walled && (y >= lava.level || y < lava.floor)) {
                out[index] = vxe::voxel::kAirState;
                continue;
            }
            // The rock under a lake, or a wall holding a neighbor's lake back.
        }
        const BiomeLook& look = m_blocks.biomes[static_cast<usize>(at.biome[columnIndex])];
        const BlockStateId columnTop = at.top[columnIndex];
        const bool rocky = columnTop == m_blocks.stone;
        if (depth < 1.0) {
            out[index] = rocky ? bandAt(x, y, z, look.banded) : columnTop;
        } else if (depth < 1.0 + look.underDepth && !rocky) {
            out[index] = look.under;
        } else if (look.deep != vxe::voxel::kAirState && depth < look.deepDepth && !rocky) {
            out[index] = look.deep;
        } else if ((rocky && depth < 7.0) || (look.banded && depth < 60.0)) {
            out[index] = bandAt(x, y, z, look.banded);
        } else {
            out[index] = stoneAt(x, y, z, fieldAt(rockA, local.x, local.y, local.z),
                                 fieldAt(rockB, local.x, local.y, local.z));
        }
    }
    if (anyStone) {
        placeOres(chunk, out);
    }
    // Things that stand on the ground only reach chunks near it.
    const i64 highest = at.highest;
    const i64 lowest = at.lowest;
    if (origin.y <= highest + kTreeHeadroom && origin.y + vxe::kChunkSize >= lowest - 8) {
        placeFeatures(chunk, out);
        placeTrees(chunk, out);
        placePlants(chunk, out);
        placeReefs(chunk, out);
    }
    coverSnow(chunk, out);
}

bool Terrain::isCave(const Column& at, usize columnIndex, i64 x, i64 y, i64 z, f64 depth, f64 tunnelA,
                     f64 tunnelB, f64 cavern) const noexcept {
    if (depth <= 1.5) {
        return false;
    }
    // Under lakes, rivers and the sea the roof stays thick, or the water would
    // stand over open air.
    const bool wet = at.water[columnIndex] > at.heights[columnIndex];
    if (wet && depth < 10.0) {
        return false;
    }
    // Tunnels: round, from two fields that are near zero together. They wind
    // on for hundreds of blocks and cross each other.
    const f64 radius = 0.085;
    const bool tunnel = (tunnelA * tunnelA) + (tunnelB * tunnelB) < radius * radius;
    if (tunnel) {
        if (depth > 6.0) {
            return true;
        }
        // Near the surface only where the ground opens up: cave mouths.
        const f64 mouth =
            vxe::noise::gradient2(m_seed + 78, static_cast<f64>(x) / 70.0, static_cast<f64>(z) / 70.0).value;
        if (!wet && mouth > 0.45) {
            return true;
        }
    }
    // Caverns: only deep down, wider the deeper they lie.
    if (depth > 40.0) {
        const f64 deeper = std::clamp((depth - 40.0) / 400.0, 0.0, 1.0);
        if (cavern > 0.62 - (deeper * 0.22)) {
            return true;
        }
    }
    // Ravines: rare long cracks from the surface down, narrowing towards
    // their floor.
    if (!wet && depth < 48.0) {
        const f64 where =
            vxe::noise::gradient2(m_seed + 77, static_cast<f64>(x) / 900.0, static_cast<f64>(z) / 900.0)
                .value;
        if (where > 0.42) {
            const f64 crack = std::abs(
                vxe::noise::gradient2(m_seed + 76, static_cast<f64>(x) / 260.0, static_cast<f64>(z) / 260.0)
                    .value);
            const f64 width = 0.016 * (1.0 - (depth / 52.0)) * std::min((where - 0.42) * 8.0, 1.0);
            if (crack < width) {
                return true;
            }
        }
    }
    static_cast<void>(y);
    return false;
}

i64 Terrain::lavaLevel(i64 regionX, i64 regionZ) const noexcept {
    const u64 h = hash3(m_seed + 97, regionX, 0, regionZ);
    if (h % 100 >= kLavaShare) {
        return kFloorY; // No lava here.
    }
    return kLavaHighest - (static_cast<i64>((h >> 16U) % kLavaSteps) * kLavaStep);
}

Terrain::LavaColumn Terrain::lavaAt(i64 x, i64 z) const noexcept {
    const i64 regionX = floorDiv(x, kLavaRegion);
    const i64 regionZ = floorDiv(z, kLavaRegion);
    const i64 level = lavaLevel(regionX, regionZ);
    LavaColumn lava{.level = level,
                    .bottom = level - kLavaDepth,
                    .floor = level - kLavaDepth - kLavaFloor,
                    .wallLow = 0,
                    .wallHigh = 0};
    const i64 localX = x - (regionX * kLavaRegion);
    const i64 localZ = z - (regionZ * kLavaRegion);
    const i64 stepX = localX < kLavaWall ? -1 : (localX >= kLavaRegion - kLavaWall ? 1 : 0);
    const i64 stepZ = localZ < kLavaWall ? -1 : (localZ >= kLavaRegion - kLavaWall ? 1 : 0);
    // Near an edge, the neighbors across it (and across a corner) count.
    const std::array<std::array<i64, 2>, 3> neighbors = {{{stepX, 0}, {0, stepZ}, {stepX, stepZ}}};
    for (const auto& [dx, dz] : neighbors) {
        if (dx != 0 || dz != 0) {
            const i64 other = lavaLevel(regionX + dx, regionZ + dz);
            if (other != level && other != kFloorY) {
                // The neighbor's lake and its floor, where this side may be open.
                const i64 low = other - kLavaDepth - kLavaFloor;
                lava.wallLow = lava.wallHigh > lava.wallLow ? std::min(lava.wallLow, low) : low;
                lava.wallHigh = std::max(lava.wallHigh, other);
            }
        }
    }
    return lava;
}

void Terrain::placeOres(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    // Each ore in its band of heights, a number of veins per chunk (in
    // hundredths) and a vein size (blocks), as in modern Minecraft: coal high,
    // copper and iron in the middle, gold deep, diamonds deepest and emeralds
    // high in the mountains.
    struct Ore {
        BlockStateId block; // In stone and the other rocks; air for deepslate only ores.
        BlockStateId deep;  // In deepslate.
        i64 lowest;
        i64 highest;
        u32 veins; // Per chunk, in hundredths.
        u32 smallest;
        u32 largest;
    };
    // Lapis and redstone only come in deepslate (their stone kinds have no
    // textures yet).
    const BlockStateId none = vxe::voxel::kAirState;
    const std::array<Ore, 8> ores = {{
        {m_blocks.coal, m_blocks.deepCoal, 0, 320, 260, 6, 15},
        {m_blocks.copper, m_blocks.deepCopper, -16, 128, 170, 5, 11},
        {m_blocks.iron, m_blocks.deepIron, -64, 256, 190, 4, 9},
        {m_blocks.gold, m_blocks.deepGold, -160, 32, 70, 4, 8},
        {m_blocks.diamond, m_blocks.deepDiamond, -1000, -24, 45, 2, 7},
        {m_blocks.emerald, m_blocks.deepEmerald, 120, 480, 40, 1, 3},
        {none, m_blocks.deepLapis, -200, 32, 80, 3, 8},
        {none, m_blocks.deepRedstone, -400, 16, 220, 4, 9},
    }};
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const auto host = [this](BlockStateId state) {
        return state == m_blocks.stone || state == m_blocks.granite || state == m_blocks.andesite ||
               state == m_blocks.tuff || state == m_blocks.basalt || state == m_blocks.calcite;
    };
    u64 rng = hash3(m_seed + 90, chunk.x, chunk.y, chunk.z);
    const auto next = [&rng] {
        rng = mix(rng + 0x9E3779B97F4A7C15ULL);
        return rng;
    };
    for (usize kind = 0; kind < ores.size(); ++kind) {
        const Ore& ore = ores.at(kind);
        if ((ore.block == none && ore.deep == none) || origin.y + vxe::kChunkSize <= ore.lowest ||
            origin.y > ore.highest) {
            continue;
        }
        u32 veins = ore.veins / 100;
        if (next() % 100 < ore.veins % 100) {
            ++veins;
        }
        for (u32 vein = 0; vein < veins; ++vein) {
            i64 x = static_cast<i64>(next() % vxe::kChunkSize);
            i64 y = static_cast<i64>(next() % vxe::kChunkSize);
            i64 z = static_cast<i64>(next() % vxe::kChunkSize);
            if (origin.y + y < ore.lowest || origin.y + y > ore.highest) {
                continue;
            }
            const u32 size = ore.smallest + static_cast<u32>(next() % (ore.largest - ore.smallest + 1));
            // A short random walk: veins come out as lumps and strands.
            for (u32 step = 0; step < size * 2 && step < 40; ++step) {
                const i64 worldY = origin.y + y;
                if (x >= 0 && y >= 0 && z >= 0 && x < vxe::kChunkSize && y < vxe::kChunkSize &&
                    z < vxe::kChunkSize && worldY >= ore.lowest && worldY <= ore.highest) {
                    const usize index = vxe::LocalPos{.x = static_cast<vxe::u8>(x),
                                                      .y = static_cast<vxe::u8>(y),
                                                      .z = static_cast<vxe::u8>(z)}
                                            .index();
                    const BlockStateId rock = out[index];
                    if (rock == m_blocks.deepslate && ore.deep != none) {
                        out[index] = ore.deep;
                    } else if (host(rock) && ore.block != none) {
                        out[index] = ore.block;
                    }
                }
                const u64 move = next();
                const i64 delta = (move & 1U) != 0U ? 1 : -1;
                switch ((move >> 1U) % 3U) {
                case 0:
                    x += delta;
                    break;
                case 1:
                    y += delta;
                    break;
                default:
                    z += delta;
                    break;
                }
                if (step + 1 >= size && (move >> 8U) % 3U == 0U) {
                    break;
                }
            }
        }
    }
}

void Terrain::placeFeatures(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const Stamp stamp(origin, out);
    const i64 firstX = floorDiv(origin.x - kFeatureReach, kFeatureCell);
    const i64 lastX = floorDiv(origin.x + vxe::kChunkSize + kFeatureReach, kFeatureCell);
    const i64 firstZ = floorDiv(origin.z - kFeatureReach, kFeatureCell);
    const i64 lastZ = floorDiv(origin.z + vxe::kChunkSize + kFeatureReach, kFeatureCell);
    for (i64 cellZ = firstZ; cellZ <= lastZ; ++cellZ) {
        for (i64 cellX = firstX; cellX <= lastX; ++cellX) {
            const u64 h = hash3(m_seed + 23, cellX, 0, cellZ);
            const i64 x = (cellX * kFeatureCell) + static_cast<i64>((h >> 8U) % kFeatureCell);
            const i64 z = (cellZ * kFeatureCell) + static_cast<i64>((h >> 20U) % kFeatureCell);
            const i64 ground = surfaceHeight(x, z);
            if (ground < waterLevel(x, z) || ground + 6 < origin.y ||
                ground - 6 > origin.y + vxe::kChunkSize) {
                continue;
            }
            const BiomeLook& look = m_blocks.biomes[static_cast<usize>(biomeAt(x, z))];
            const f64 roll = unit(mix(h));
            if (roll < static_cast<f64>(look.boulders)) {
                // A rough lump half sunk into the ground, of two kinds of rock.
                const i64 rx = 1 + static_cast<i64>((h >> 32U) % 3U);
                const i64 rz = 1 + static_cast<i64>((h >> 36U) % 3U);
                const i64 ry = 1 + static_cast<i64>((h >> 40U) % 2U);
                for (i64 dy = -1; dy <= ry; ++dy) {
                    for (i64 dz = -rz; dz <= rz; ++dz) {
                        for (i64 dx = -rx; dx <= rx; ++dx) {
                            const f64 fx = static_cast<f64>(dx) / (static_cast<f64>(rx) + 0.5);
                            const f64 fy = static_cast<f64>(dy) / (static_cast<f64>(ry) + 0.5);
                            const f64 fz = static_cast<f64>(dz) / (static_cast<f64>(rz) + 0.5);
                            const u64 block = hash3(h, x + dx, ground + dy, z + dz);
                            if ((fx * fx) + (fy * fy) + (fz * fz) + (unit(block) * 0.3) < 1.0) {
                                stamp.put(x + dx, ground + dy + 1, z + dz,
                                          (block & 3U) == 0U ? look.boulderMix : look.boulder, true);
                            }
                        }
                    }
                }
            } else if (roll < static_cast<f64>(look.boulders + look.fallenLogs)) {
                // A fallen trunk, only where the ground is level enough for it.
                const bool alongX = (h & 1U) != 0U;
                const i64 length = 3 + static_cast<i64>((h >> 44U) % 4U);
                const i64 endX = x + (alongX ? length : 0);
                const i64 endZ = z + (alongX ? 0 : length);
                if (std::abs(surfaceHeight(endX, endZ) - ground) > 1) {
                    continue;
                }
                const TreeBlocks& wood = m_blocks.trees[static_cast<usize>(look.logKind)];
                for (i64 step = 0; step <= length; ++step) {
                    stamp.put(x + (alongX ? step : 0), ground + 1, z + (alongX ? 0 : step),
                              alongX ? wood.logX : wood.logZ, false);
                }
            }
        }
    }
}

std::optional<Terrain::Tree> Terrain::treeIn(i64 cellX, i64 cellZ, Biome biome, f64 damp) const noexcept {
    const BiomeLook& look = m_blocks.biomes[static_cast<usize>(biome)];
    if (look.treeCount == 0 && damp <= 0.0) {
        return std::nullopt;
    }
    const u64 h = hash3(m_seed + 17, cellX, 0, cellZ);
    auto chance = static_cast<f64>(look.trees);
    if (look.groves > 0.0F) {
        // Groups and clearings: more trees where slow noise is high, none where
        // it is low, so that forests have edges and plains have lone groves.
        const f64 wood =
            vxe::noise::gradient2(m_seed + 19, static_cast<f64>(cellX) / 9.0, static_cast<f64>(cellZ) / 9.0)
                .value;
        const f64 grove = smooth(-0.15, 0.4, wood) * 2.4;
        chance *= 1.0 + ((grove - 1.0) * static_cast<f64>(look.groves));
    }
    // Trees crowd along water. A biome with none of its own grows oaks there.
    chance += damp * (look.dry ? 0.09 : 0.32);
    if (unit(h) >= chance) {
        return std::nullopt;
    }
    const TreeChoice* choice =
        pickWeighted(std::span<const TreeChoice>(look.treeKinds.data(), look.treeCount), unit(mix(h)));
    Tree tree;
    tree.kind = choice != nullptr ? choice->kind : TreeKind::Oak;
    if (look.waterTree != TreeKind::Oak && damp > 0.05) {
        tree.kind = look.waterTree;
    }
    tree.seed = mix(h + 5);
    tree.x = (cellX * kTreeCell) + static_cast<i64>((h >> 8U) % kTreeCell);
    tree.z = (cellZ * kTreeCell) + static_cast<i64>((h >> 16U) % kTreeCell);
    tree.crownX = tree.x;
    tree.crownZ = tree.z;
    const auto size = static_cast<i64>((h >> 24U) % 256U);
    switch (tree.kind) {
    case TreeKind::Oak:
        tree.trunk = 4 + (size % 3);
        tree.height = tree.trunk + 1;
        tree.crownDepth = 4;
        tree.radius = 2;
        break;
    case TreeKind::Birch:
        tree.trunk = 6 + (size % 3);
        tree.height = tree.trunk + 1;
        tree.crownDepth = 4;
        tree.radius = 2;
        break;
    case TreeKind::TallBirch:
        tree.trunk = 10 + (size % 4);
        tree.height = tree.trunk + 1;
        tree.crownDepth = 4;
        tree.radius = 2;
        break;
    case TreeKind::Jungle:
        tree.trunk = 6 + (size % 4);
        tree.height = tree.trunk + 1;
        tree.crownDepth = 4;
        tree.radius = 2;
        break;
    case TreeKind::BigOak:
        tree.trunk = 8 + (size % 4);
        tree.height = tree.trunk + 3;
        tree.crownDepth = 7;
        tree.radius = 5;
        break;
    case TreeKind::Spruce:
        tree.height = 9 + (size % 8);
        tree.trunk = tree.height - 1;
        tree.crownDepth = tree.height - 2 - (size % 2);
        tree.radius = tree.height > 12 ? 3 : 2;
        break;
    case TreeKind::TallSpruce:
        tree.height = 18 + (size % 11);
        tree.trunk = tree.height - 1;
        tree.crownDepth = tree.height - 7 - (size % 3);
        tree.radius = 3 + (size % 2);
        break;
    case TreeKind::Pine:
        tree.height = 11 + (size % 6);
        tree.trunk = tree.height - 1;
        tree.crownDepth = 5;
        tree.radius = 2;
        break;
    case TreeKind::DarkOak:
    case TreeKind::PaleOak:
        tree.trunk = 6 + (size % 3);
        tree.height = tree.trunk + 2;
        tree.crownDepth = 4;
        tree.radius = 5;
        break;
    case TreeKind::Acacia: {
        tree.trunk = 6 + (size % 2);
        tree.height = tree.trunk + 2;
        tree.crownDepth = 2;
        tree.radius = 4;
        const i64 way = static_cast<i64>((h >> 34U) % 4U);
        tree.crownX += way == 0 ? 3 : (way == 1 ? -3 : 1);
        tree.crownZ += way == 2 ? 3 : (way == 3 ? -3 : -1);
        break;
    }
    case TreeKind::GiantJungle:
        tree.trunk = 15 + (size % 7);
        tree.height = tree.trunk + 3;
        tree.crownDepth = 7;
        tree.radius = 6;
        break;
    case TreeKind::Azalea:
        tree.trunk = 3 + (size % 2);
        tree.height = tree.trunk + 2;
        tree.crownDepth = 5;
        tree.radius = 3;
        break;
    case TreeKind::Bush:
        tree.trunk = 1;
        tree.height = 3;
        tree.crownDepth = 3;
        tree.radius = 2;
        break;
    case TreeKind::Palm: {
        tree.trunk = 8 + (size % 6);
        tree.height = tree.trunk + 1;
        tree.crownDepth = 4;
        tree.radius = 5;
        const i64 way = static_cast<i64>((h >> 34U) % 4U);
        tree.crownX += way == 0 ? 2 : (way == 1 ? -2 : 1);
        tree.crownZ += way == 2 ? 2 : (way == 3 ? -2 : -1);
        break;
    }
    case TreeKind::Olive:
        tree.trunk = 2 + (size % 2);
        tree.height = 7 + (size % 2);
        tree.crownDepth = 4;
        tree.radius = 4;
        break;
    case TreeKind::Maple:
    case TreeKind::GoldMaple:
        tree.trunk = 6 + (size % 3);
        tree.height = tree.trunk + 5;
        tree.crownDepth = 8;
        tree.radius = 4;
        break;
    case TreeKind::Hemlock:
        tree.height = 16 + (size % 9);
        tree.trunk = tree.height - 1;
        tree.crownDepth = tree.height - 3;
        tree.radius = 3;
        break;
    case TreeKind::Poplar:
        tree.height = 14 + (size % 7);
        tree.trunk = tree.height - 2;
        tree.crownDepth = tree.height - 3;
        tree.radius = 2;
        break;
    case TreeKind::WeepingBirch:
        tree.trunk = 8 + (size % 4);
        tree.height = tree.trunk + 2;
        tree.crownDepth = 6;
        tree.radius = 4;
        break;
    case TreeKind::ForkedBirch:
        tree.trunk = 8 + (size % 4);
        tree.height = tree.trunk + 2;
        tree.crownDepth = 5;
        tree.radius = 4;
        break;
    }
    return tree;
}

std::optional<Terrain::PlacedTree> Terrain::treeAt(i64 cellX, i64 cellZ) const {
    // The spot comes from the same hash as in treeIn, so the biome can be read
    // there first.
    const u64 h = hash3(m_seed + 17, cellX, 0, cellZ);
    const i64 x = (cellX * kTreeCell) + static_cast<i64>((h >> 8U) % kTreeCell);
    const i64 z = (cellZ * kTreeCell) + static_cast<i64>((h >> 16U) % kTreeCell);
    const Column& at = columnAt(x, z);
    const auto index = static_cast<usize>(((z & 15) * vxe::kChunkSize) + (x & 15));
    const BiomeLook& look = m_blocks.biomes[static_cast<usize>(at.biome[index])];
    // Palms and olives root in the sand their biomes are made of.
    const bool sandy =
        look.waterTree != TreeKind::Oak && at.top[index] == look.top && at.heights[index] >= at.water[index];
    if ((!at.soil[index] && !sandy) || at.lean[index] > 0.0F) {
        return std::nullopt;
    }
    const i64 ground = at.heights[index];
    const std::optional<Tree> tree = treeIn(cellX, cellZ, at.biome[index], static_cast<f64>(at.damp[index]));
    if (!tree) {
        return std::nullopt;
    }
    // A palm belongs on a warm shore. Asked only for palms, since it costs a climate look.
    if (tree->kind == TreeKind::Palm &&
        m_climate.sample(static_cast<f64>(x), static_cast<f64>(z), 0.0).temperature < kPalmWarmth) {
        return std::nullopt;
    }
    return PlacedTree{.tree = *tree, .ground = ground};
}

void Terrain::placeTrees(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const Stamp stamp(origin, out);
    const i64 firstCellX = floorDiv(origin.x - kTreeReach, kTreeCell);
    const i64 lastCellX = floorDiv(origin.x + vxe::kChunkSize + kTreeReach, kTreeCell);
    const i64 firstCellZ = floorDiv(origin.z - kTreeReach, kTreeCell);
    const i64 lastCellZ = floorDiv(origin.z + vxe::kChunkSize + kTreeReach, kTreeCell);
    for (i64 cellZ = firstCellZ; cellZ <= lastCellZ; ++cellZ) {
        for (i64 cellX = firstCellX; cellX <= lastCellX; ++cellX) {
            const std::optional<PlacedTree> placed = treeAt(cellX, cellZ);
            if (!placed || placed->ground + placed->tree.height + 1 < origin.y ||
                placed->ground > origin.y + vxe::kChunkSize) {
                continue;
            }
            const TreeBlocks& wood = m_blocks.trees[static_cast<usize>(placed->tree.kind)];
            buildTree(TreeStamp(stamp, wood, placed->tree.seed), placed->tree, placed->ground);
        }
    }
}

const PlantChoice* Terrain::plantAt(i64 x, i64 z, const BiomeLook& look, f64 damp) const noexcept {
    const u64 h = hash3(m_seed + 67, x, 0, z);
    const f64 roll = unit(h);
    const auto wx = static_cast<f64>(x);
    const auto wz = static_cast<f64>(z);
    // Where water feeds the ground plants crowd in, whatever the biome.
    const f64 thrive = look.dry ? kDryWatersidePlants : kWatersidePlants;
    if (damp > 0.0 && look.top != m_blocks.soils[6] && unit(mix(h + 2)) < damp * thrive) {
        return pickWeighted(std::span<const PlantChoice>(m_blocks.waterside.data(), m_blocks.watersideCount),
                            unit(mix(h + 3)));
    }
    if (look.flowers > 0.0F && roll < 0.3 && look.flowerCount > 0 &&
        vxe::noise::gradient2(m_seed + 71, wx / 13.0, wz / 13.0).value >
            0.5 - (static_cast<f64>(look.flowers) * 1.05)) {
        // Each patch favors one kind, with a few others among it.
        const u64 patch = hash3(m_seed + 73, floorDiv(x, 22), 0, floorDiv(z, 22));
        const f64 pick = (h >> 40U) % 4U == 0U ? unit(mix(h)) : unit(patch);
        return pickWeighted(std::span<const PlantChoice>(look.flowerKinds.data(), look.flowerCount), pick);
    }
    if (look.grassCount == 0 || look.grass <= 0.0F) {
        return nullptr;
    }
    // Grass grows in thick and thin stretches.
    const f64 lush = 1.0 + (vxe::noise::gradient2(m_seed + 79, wx / 21.0, wz / 21.0).value * 2.2);
    if (unit(mix(h)) >= static_cast<f64>(look.grass) * lush) {
        return nullptr;
    }
    return pickWeighted(std::span<const PlantChoice>(look.grassKinds.data(), look.grassCount),
                        unit(mix(h + 1)));
}

void Terrain::placePlants(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    const Column& at = column(chunk.x, chunk.z);
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const auto write = [&](i64 x, i64 y, i64 z, BlockStateId state) {
        out[vxe::LocalPos{.x = static_cast<u8>(x), .y = static_cast<u8>(y), .z = static_cast<u8>(z)}
                .index()] = state;
    };
    const auto read = [&](i64 x, i64 y, i64 z) {
        return out[vxe::LocalPos{.x = static_cast<u8>(x), .y = static_cast<u8>(y), .z = static_cast<u8>(z)}
                       .index()];
    };
    for (i64 z = 0; z < vxe::kChunkSize; ++z) {
        for (i64 x = 0; x < vxe::kChunkSize; ++x) {
            const auto index = static_cast<usize>((z * vxe::kChunkSize) + x);
            const i64 y = at.heights[index] + 1 - origin.y;
            // y == -1: only the top half of a tall plant can fall in this chunk.
            if (y < -1 || y >= vxe::kChunkSize || !at.soil[index] || at.lean[index] > 0.0F) {
                continue;
            }
            const BiomeLook& look = m_blocks.biomes[static_cast<usize>(at.biome[index])];
            const PlantChoice* plant =
                plantAt(origin.x + x, origin.z + z, look, static_cast<f64>(at.damp[index]));
            if (plant == nullptr) {
                continue;
            }
            if (y >= 0 && read(x, y, z) != vxe::voxel::kAirState) {
                continue;
            }
            if (y >= 0) {
                write(x, y, z, plant->state);
            }
            if (plant->upper != vxe::voxel::kAirState && y + 1 < vxe::kChunkSize &&
                read(x, y + 1, z) == vxe::voxel::kAirState) {
                write(x, y + 1, z, plant->upper);
            }
        }
    }
}

void Terrain::placeReefs(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    // Warm seas grow reefs: colonies of one color each (a few blocks across),
    // with mounds of coral blocks, and corals and fans on them and on the
    // sand between. Lukewarm seas have smaller, scattered colonies. Corals
    // stand in water (waterlogged), so they only go where water is on every
    // side but the floor, a block under the surface at least.
    const Column& at = column(chunk.x, chunk.z);
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const auto index = [](i64 x, i64 y, i64 z) {
        return vxe::LocalPos{.x = static_cast<u8>(x), .y = static_cast<u8>(y), .z = static_cast<u8>(z)}
            .index();
    };
    for (i64 z = 0; z < vxe::kChunkSize; ++z) {
        for (i64 x = 0; x < vxe::kChunkSize; ++x) {
            const auto column = static_cast<usize>((z * vxe::kChunkSize) + x);
            const Biome biome = at.biome[column];
            const bool warm = biome == Biome::WarmOcean;
            if (!warm && biome != Biome::LukewarmOcean) {
                continue;
            }
            const i64 floor = at.heights[column];
            const i64 surface = at.water[column];
            if (surface - floor < 4 || floor + 1 > origin.y + vxe::kChunkSize || surface < origin.y) {
                continue;
            }
            const i64 wx = origin.x + x;
            const i64 wz = origin.z + z;
            const auto fx = static_cast<f64>(wx);
            const auto fz = static_cast<f64>(wz);
            // Colonies: patches of reef, each one color.
            if (!inPatch(m_seed + 98, fx, fz, warm ? 0.5 : 0.18)) {
                continue;
            }
            const u64 colony = hash3(m_seed + 99, floorDiv(wx, 7), 0, floorDiv(wz, 7));
            const usize color = colony % 5U;
            const u64 h = hash3(m_seed + 100, wx, 0, wz);
            // A mound of coral blocks, up to 3 high, never within 2 of the surface.
            i64 mound = 0;
            if (h % 100U < (warm ? 22U : 8U)) {
                mound = std::min<i64>(1 + static_cast<i64>((h >> 8U) % 3U), surface - floor - 3);
            }
            for (i64 y = floor + 1; y <= floor + mound; ++y) {
                if (y >= origin.y && y < origin.y + vxe::kChunkSize) {
                    out[index(x, y - origin.y, z)] = m_blocks.coralBlocks.at(color);
                }
            }
            // A coral or a fan on top.
            const u64 grow = (h >> 16U) % 100U;
            if (grow >= (warm ? 55U : 30U)) {
                continue;
            }
            const i64 y = floor + mound + 1;
            if (y < origin.y || y >= origin.y + vxe::kChunkSize || y > surface - 1) {
                continue;
            }
            const usize cell = index(x, y - origin.y, z);
            if (out[cell] != m_blocks.water) {
                continue;
            }
            out[cell] = (h >> 24U) % 3U == 0U ? m_blocks.coralFans.at(color) : m_blocks.corals.at(color);
        }
    }
}

vxe::voxel::BlockStateId Terrain::snowLayerAt(i64 x, i64 z, vxe::u8 depth) const noexcept {
    // Drifts: deeper and thinner stretches across the ground.
    const f64 drift =
        (vxe::noise::gradient2(m_seed + 83, static_cast<f64>(x) / 11.0, static_cast<f64>(z) / 11.0).value +
         1.0) *
        0.5;
    const auto layers = std::clamp<i64>(1 + static_cast<i64>(drift * static_cast<f64>(depth)), 1, depth);
    return m_blocks.snowLayers.at(static_cast<usize>(layers - 1));
}

vxe::voxel::BlockStateId Terrain::iceAt(i64 x, i64 z, Biome biome) const noexcept {
    // Lakes and rivers freeze over; the sea in sheets with open leads between
    // them and now and then a slab of packed ice.
    if (biome != Biome::FrozenOcean) {
        return m_blocks.ice;
    }
    const auto wx = static_cast<f64>(x);
    const auto wz = static_cast<f64>(z);
    if (!inPatch(m_seed + 89, wx, wz, 0.78)) {
        return m_blocks.water;
    }
    return inPatch(m_seed + 97, wx, wz, 0.1) ? m_blocks.packedIce : m_blocks.ice;
}

void Terrain::coverSnow(vxe::ChunkPos chunk, std::span<BlockStateId, vxe::kChunkVolume> out) const {
    // Snow falls on the highest block of each column, so it is found from the
    // top of the chunk down. A column whose highest block is the top of the
    // chunk gets its snow from the chunk above (where the ground is known), so
    // every chunk decides alone. Under an overhang, in a cave or on bare steep
    // rock no snow lies.
    const Column& at = column(chunk.x, chunk.z);
    const vxe::BlockPos origin = vxe::chunkOrigin(chunk);
    const auto cell = [](i64 x, i64 y, i64 z) {
        return vxe::LocalPos{.x = static_cast<u8>(x), .y = static_cast<u8>(y), .z = static_cast<u8>(z)}
            .index();
    };
    for (i64 z = 0; z < vxe::kChunkSize; ++z) {
        for (i64 x = 0; x < vxe::kChunkSize; ++x) {
            const auto index = static_cast<usize>((z * vxe::kChunkSize) + x);
            const Biome biome = at.biome[index];
            const vxe::u8 depth = m_blocks.biomes[static_cast<usize>(biome)].snowDepth;
            if (depth == 0 || at.lean[index] > 0.0F) {
                continue;
            }
            const i64 wx = origin.x + x;
            const i64 wz = origin.z + z;
            const bool bareRock = at.top[index] == m_blocks.stone;
            i64 y = vxe::kChunkSize - 1;
            while (y >= 0 && out[cell(x, y, z)] == vxe::voxel::kAirState) {
                --y;
            }
            if (y < 0) {
                // Open all the way down: snow on the ground just below the chunk.
                if (at.heights[index] == origin.y - 1 && at.heights[index] >= at.water[index] && !bareRock) {
                    out[cell(x, 0, z)] = snowLayerAt(wx, wz, depth);
                }
                continue;
            }
            const i64 worldY = origin.y + y;
            const BlockStateId state = out[cell(x, y, z)];
            if (worldY < at.heights[index] && state != m_blocks.water) {
                continue; // A cave.
            }
            if (state == m_blocks.water) {
                if (worldY == at.water[index]) {
                    out[cell(x, y, z)] = iceAt(wx, wz, biome);
                }
                continue;
            }
            const SnowRest rest =
                state < m_blocks.snowRest.size() ? m_blocks.snowRest[state] : SnowRest::None;
            if (rest == SnowRest::Buries) {
                // Grass and flowers vanish under it, both halves of a tall one.
                i64 base = y;
                while (base > 0 && m_blocks.snowRest[out[cell(x, base - 1, z)]] == SnowRest::Buries) {
                    --base;
                }
                for (i64 above = base + 1; above <= y; ++above) {
                    out[cell(x, above, z)] = vxe::voxel::kAirState;
                }
                out[cell(x, base, z)] = snowLayerAt(wx, wz, depth);
                continue;
            }
            if (rest != SnowRest::Holds || y == vxe::kChunkSize - 1 ||
                (bareRock && worldY == at.heights[index])) {
                continue;
            }
            // Leaves hold a thin layer; the ground gathers drifts.
            out[cell(x, y + 1, z)] =
                worldY > at.heights[index] ? m_blocks.snowLayers[0] : snowLayerAt(wx, wz, depth);
        }
    }
}

} // namespace vr
