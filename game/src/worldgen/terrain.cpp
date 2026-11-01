#include "terrain.hpp"

#include <vxe/core/hash.hpp>
#include <vxe/math/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
constexpr f64 kRockSlope = 1.6;        // Steeper ground than this holds no soil.
constexpr f64 kSnowSlope = 3.4;        // Steeper ground than this holds no snow.
constexpr f64 kOverhangMost = 12.0;    // Blocks a rock face leans at most.
constexpr i64 kTreeHeadroom = 32;      // The tallest tree, above any ground.
constexpr i64 kFeatureCell = 19;       // Blocks per side of a boulder or fallen log cell.
constexpr i64 kFeatureReach = 7;       // How far those reach from their cell's spot.
constexpr usize kCacheColumns = 256;   // Per thread, a power of two.
constexpr f64 kFarRiverCell = 4.0;     // Far cells wider than this leave rivers and lakes out.
constexpr f64 kGreenDamp = 0.45;       // How damp ground must be to turn green by itself.
constexpr f64 kWatersidePlants = 0.72; // Share of the wettest ground under plants.
constexpr i64 kBandHeight = 5;         // Blocks of one rock layer.

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
        const i64 crownTop = ground + tree.height;
        for (i64 layer = 0; layer < tree.crownDepth; ++layer) {
            const i64 wide = std::min<i64>((layer + 2) / 3 + 1, tree.radius);
            builder.disc(x, crownTop - layer, z, layer == 0 ? 0 : (layer % 2 == 0 ? wide : wide - 1), false);
        }
        trunk(builder, x, ground, z, crownTop, tree.kind == TreeKind::TallSpruce && tree.height > 23);
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
    , m_identity(mix(seed) ^ vxe::hashBytes(std::as_bytes(std::span(&preset, 1))))
    , m_planet(seed, preset)
    , m_climate(seed, m_planet)
    , m_rivers(m_planet)
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
    if (!underWater && at.damp > kGreenDamp && look.top != snow && look.top != m_blocks.soils[6]) {
        return m_blocks.soils[0];
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
    if (!entry.valid || entry.identity != m_identity || entry.chunkX != chunkX || entry.chunkZ != chunkZ) {
        buildColumn(chunkX, chunkZ, entry);
    }
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
    // Ores by depth, then rock kinds in large blobs from two slow fields.
    const f64 roll = unit(hash3(m_seed + 3, x, y, z));
    if (roll < 0.012) {
        return m_blocks.coal;
    }
    if (roll < 0.018 && y < 48) {
        return m_blocks.iron;
    }
    if (roll < 0.0205 && y < 24) {
        return m_blocks.gold;
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
    if (anyStone) {
        fillField(rockA, origin, [&](f64 x, f64 y, f64 z) {
            return vxe::noise::gradient3(m_seed + 5, x / 34.0, y / 22.0, z / 34.0);
        });
        fillField(rockB, origin, [&](f64 x, f64 y, f64 z) {
            return vxe::noise::gradient3(m_seed + 6, x / 41.0, y / 26.0, z / 41.0);
        });
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
        // Caves: winding tunnels where 3D noise is near zero, kept away from
        // the surface so lakes do not drain into them.
        if (depth > 6.0) {
            const f64 cave = vxe::noise::gradient3(m_seed + 11, static_cast<f64>(x) / 18.0,
                                                   static_cast<f64>(y) / 12.0, static_cast<f64>(z) / 18.0);
            if (std::abs(cave) < 0.05) {
                out[index] = vxe::voxel::kAirState;
                continue;
            }
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
    // Things that stand on the ground only reach chunks near it.
    const i64 highest = at.highest;
    const i64 lowest = at.lowest;
    if (origin.y <= highest + kTreeHeadroom && origin.y + vxe::kChunkSize >= lowest - 8) {
        placeFeatures(chunk, out);
        placeTrees(chunk, out);
        placePlants(chunk, out);
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
    chance += damp * 0.32;
    if (unit(h) >= chance) {
        return std::nullopt;
    }
    const TreeChoice* choice =
        pickWeighted(std::span<const TreeChoice>(look.treeKinds.data(), look.treeCount), unit(mix(h)));
    Tree tree;
    tree.kind = choice != nullptr ? choice->kind : TreeKind::Oak;
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
    if (!at.soil[index] || at.lean[index] > 0.0F) {
        return std::nullopt;
    }
    const i64 ground = at.heights[index];
    const std::optional<Tree> tree = treeIn(cellX, cellZ, at.biome[index], static_cast<f64>(at.damp[index]));
    if (!tree) {
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
    if (damp > 0.0 && look.top != m_blocks.soils[6] && unit(mix(h + 2)) < damp * kWatersidePlants) {
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

} // namespace vr
