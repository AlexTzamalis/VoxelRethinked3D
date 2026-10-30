#include "planet.hpp"

#include <vxe/math/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace vr {
namespace {

using vxe::f64;
using vxe::i64;
using vxe::u64;
using vxe::usize;
using vxe::noise::Fractal;
using vxe::noise::smooth;
using vxe::noise::unit;

constexpr f64 kPlateJitter = 0.9;
constexpr f64 kWarpReach = 900.0;   // Blocks that coasts and borders wander.
constexpr f64 kBorderReach = 420.0; // Blocks that plate borders also wander, more finely.
constexpr f64 kCoastShelf = 0.14;   // Continent values from the shelf edge to the beach.
constexpr f64 kLowland = 0.45;      // Continent value where the land stops rising by itself.
constexpr f64 kCoastRise = 16.0;    // Blocks the land climbs right behind the beach.
constexpr f64 kInlandHeight = 62.0;
constexpr f64 kRiftDepth = 46.0;
constexpr f64 kSeaReach = 0.42; // How much of a range survives out at sea.
constexpr f64 kMeanCrest = 0.6; // A range's usual height, as a part of its tallest.
constexpr vxe::u32 kContinentOctaves = 6;
constexpr vxe::u32 kBarrierOctaves = 3;

// Named streams of randomness out of one seed, so that each field is
// independent of the others.
u64 stream(u64 seed, u64 salt) noexcept {
    return vxe::noise::hash2(seed, static_cast<i64>(salt), 0x5EED);
}

struct Plate {
    f64 x = 0.0;
    f64 z = 0.0;
    f64 moveX = 0.0;
    f64 moveZ = 0.0;
    f64 distance = 0.0;
};

} // namespace

Planet::Planet(u64 seed, const PlanetPreset& preset) noexcept
    : m_preset(preset)
    , m_warpX(stream(seed, 1))
    , m_warpZ(stream(seed, 2))
    , m_continent(stream(seed, 3))
    , m_plates(stream(seed, 4))
    , m_ridges(stream(seed, 5))
    , m_giants(stream(seed, 6))
    , m_hills(stream(seed, 7)) {}

Planet::Tectonics Planet::tectonics(f64 x, f64 z) const noexcept {
    const f64 px = x / m_preset.plateSize;
    const f64 pz = z / m_preset.plateSize;
    const auto cellX = static_cast<i64>(std::floor(px));
    const auto cellZ = static_cast<i64>(std::floor(pz));
    // Every plate that can matter: the window reaches two cells past this one,
    // and the nearest plate is at most 1.42 cells away.
    std::array<Plate, 25> plates;
    usize plateCount = 0;
    f64 closest = 1.0e30;
    for (i64 dz = -2; dz <= 2; ++dz) {
        for (i64 dx = -2; dx <= 2; ++dx) {
            const vxe::noise::WorleyCell cell =
                vxe::noise::worleyPoint(m_plates, cellX + dx, cellZ + dz, kPlateJitter);
            const f64 ax = cell.pointX - px;
            const f64 az = cell.pointZ - pz;
            const f64 distance = std::sqrt((ax * ax) + (az * az));
            closest = std::min(closest, distance);
            plates[plateCount++] = {.x = cell.pointX,
                                    .z = cell.pointZ,
                                    .moveX = unit(cell.id) - 0.5,
                                    .moveZ = unit(cell.id * 0x9E3779B97F4A7C15ULL) - 0.5,
                                    .distance = distance};
        }
    }
    // A border is where two plates are both about as near as the nearest one.
    // Summing over all pairs by that measure has no order of plates in it, so
    // nothing can jump where the nearest and second nearest trade places: a
    // border simply fades out as a third plate cuts in, and ranges taper off
    // at junctions. Far plates add exactly 0.
    const f64 reach = 2.0 * m_preset.rangeWidth / m_preset.plateSize;
    Tectonics result;
    for (usize a = 0; a < plateCount; ++a) {
        const f64 fromA = plates[a].distance - closest;
        if (fromA >= reach) {
            continue;
        }
        for (usize b = a + 1; b < plateCount; ++b) {
            const f64 apart = fromA + plates[b].distance - closest;
            if (apart >= reach) {
                continue;
            }
            const f64 gapX = plates[b].x - plates[a].x;
            const f64 gapZ = plates[b].z - plates[a].z;
            const f64 gap = std::sqrt((gapX * gapX) + (gapZ * gapZ));
            const f64 closing = (((plates[a].moveX - plates[b].moveX) * gapX) +
                                 ((plates[a].moveZ - plates[b].moveZ) * gapZ)) /
                                gap;
            const f64 near = 1.0 - smooth(0.0, reach, apart);
            // The stronger of the pairs wins, softly, so ranges merge.
            result.range = 1.0 - ((1.0 - result.range) * (1.0 - (smooth(0.02, 0.35, closing) * near)));
            result.rift = 1.0 - ((1.0 - result.rift) * (1.0 - (smooth(0.1, 0.55, -closing) * near)));
        }
    }
    return result;
}

Planet::Mass Planet::mass(f64 x, f64 z, vxe::u32 continentOctaves) const noexcept {
    // Warping bends coasts and plate borders, which would otherwise be
    // smooth noise contours and straight lines.
    const Fractal warpShape{.octaves = 3, .frequency = 1.0 / 5200.0};
    const f64 wx = x + (vxe::noise::fbm2(m_warpX, x, z, warpShape).value * kWarpReach);
    const f64 wz = z + (vxe::noise::fbm2(m_warpZ, x, z, warpShape).value * kWarpReach);

    const Fractal continentShape{
        .octaves = continentOctaves, .frequency = 1.0 / m_preset.continentSize, .gain = 0.52};
    const f64 continent =
        (vxe::noise::fbm2(m_continent, wx, wz, continentShape).value * 1.25) + m_preset.landBias;

    // Plate borders wander at a finer scale than coasts, so none run straight.
    const Fractal borderShape{.octaves = 3, .frequency = 1.0 / 1700.0};
    const Tectonics plates =
        tectonics(wx + (vxe::noise::fbm2(m_warpZ + 1, x, z, borderShape).value * kBorderReach),
                  wz + (vxe::noise::fbm2(m_warpX + 1, x, z, borderShape).value * kBorderReach));

    // Ranges are as strong as the plates push, and come and go along a border,
    // which leaves passes and natural ends.
    const f64 giant = smooth(
        0.12, 0.42, vxe::noise::fbm2(m_giants, wx, wz, {.octaves = 3, .frequency = 1.0 / 9000.0}).value);
    const f64 along = smooth(
        -0.5, -0.05, vxe::noise::fbm2(m_giants + 1, wx, wz, {.octaves = 3, .frequency = 1.0 / 3400.0}).value);
    const f64 pushed = plates.range * along;
    return {.warpedX = wx,
            .warpedZ = wz,
            .continent = continent,
            .inland = smooth(-0.02, 0.1, continent),
            .strength = pushed * pushed * (3.0 - (2.0 * pushed)),
            .tall = m_preset.rangeHeight * (1.0 + (giant * (m_preset.giantFactor - 1.0))),
            .rift = plates.rift};
}

PlanetSample Planet::sample(f64 x, f64 z) const noexcept {
    const Mass land = mass(x, z, kContinentOctaves);
    const f64 continent = land.continent;

    // Land height with no mountains: the deep sea floor, a shelf up to the
    // coast, then ground that climbs from the beach and slowly on inland.
    f64 base = 0.0;
    if (continent < 0.0) {
        base = (-m_preset.oceanDepth * (1.0 - smooth(-0.38, 0.0, continent))) -
               (8.0 * smooth(0.0, kCoastShelf, -continent));
    } else {
        base = (2.0 * smooth(0.0, 0.015, continent)) + (kCoastRise * smooth(0.0, 0.07, continent)) +
               (kInlandHeight * smooth(0.1, kLowland, continent));
    }

    const Fractal ridgeShape{.octaves = 7, .frequency = 1.0 / 1500.0, .gain = 0.52};
    const f64 ridge = vxe::noise::ridged2(m_ridges, land.warpedX, land.warpedZ, ridgeShape).value;
    // At sea, the same collisions break the surface now and then: island arcs.
    const f64 reach = kSeaReach + ((1.0 - kSeaReach) * land.inland);
    // Crests grow with the push, so a range rises out of foothills instead of
    // standing on the plain as a slab.
    const f64 mountain = land.strength * reach * (0.06 + (ridge * (0.35 + (0.75 * land.strength))));

    // Rolling hills inland, and rifts that sink the land along parting plates.
    const Fractal hillShape{.octaves = 5, .frequency = 1.0 / 1100.0};
    const f64 hills = vxe::noise::fbm2(m_hills, land.warpedX, land.warpedZ, hillShape).value *
                      m_preset.hillHeight * land.inland;
    const f64 riftDrop = land.rift * kRiftDepth * land.inland;

    return {.elevation = base + (mountain * land.tall) + hills - riftDrop,
            .continent = continent,
            .mountain = std::min(mountain, 1.0),
            .rift = land.rift};
}

f64 Planet::barrier(f64 x, f64 z) const noexcept {
    const Mass land = mass(x, z, kBarrierOctaves);
    return land.strength * (kSeaReach + ((1.0 - kSeaReach) * land.inland)) * land.tall * kMeanCrest;
}

f64 Planet::landMass(f64 x, f64 z) const noexcept {
    return mass(x, z, kBarrierOctaves).continent;
}

} // namespace vr
