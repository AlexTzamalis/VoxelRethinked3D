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
constexpr f64 kCoastShelf = 0.05; // Continent values from the beach to the shelf's edge.
constexpr f64 kLowland = 0.45;    // Continent value where the land stops rising by itself.
constexpr f64 kCoastRise = 16.0;  // Blocks the land climbs right behind the beach.
constexpr f64 kSeaReach = 0.5;    // How much of a range survives near a coast, at sea.
constexpr f64 kMeanCrest = 0.7;   // A range's usual height, as a part of its tallest.
constexpr vxe::u32 kContinentOctaves = 8;
constexpr vxe::u32 kBarrierOctaves = 4;

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
    const Fractal warpShape{.octaves = 3, .frequency = 5.0 / m_preset.continentSize};
    const f64 warpReach = m_preset.continentSize * 0.037;
    const f64 wx = x + (vxe::noise::fbm2(m_warpX, x, z, warpShape).value * warpReach);
    const f64 wz = z + (vxe::noise::fbm2(m_warpZ, x, z, warpShape).value * warpReach);

    const Fractal continentShape{
        .octaves = continentOctaves, .frequency = 1.0 / m_preset.continentSize, .gain = 0.52};
    const f64 continent =
        (vxe::noise::fbm2(m_continent, wx, wz, continentShape).value * 1.25) + m_preset.landBias;

    // Plate borders wander at a finer scale than coasts, so none run straight.
    const Fractal borderShape{.octaves = 3, .frequency = 4.0 / m_preset.plateSize};
    const f64 borderReach = m_preset.plateSize * 0.058;
    const Tectonics plates =
        tectonics(wx + (vxe::noise::fbm2(m_warpZ + 1, x, z, borderShape).value * borderReach),
                  wz + (vxe::noise::fbm2(m_warpX + 1, x, z, borderShape).value * borderReach));

    // Ranges are as strong as the plates push and fade only over long
    // distances, so they run on for tens of kilometers. Giants are rare.
    const f64 giant = smooth(
        0.28, 0.5,
        vxe::noise::fbm2(m_giants, wx, wz, {.octaves = 3, .frequency = 2.3 / m_preset.continentSize}).value);
    const f64 along = smooth(
        -0.55, -0.1,
        vxe::noise::fbm2(m_giants + 1, wx, wz, {.octaves = 3, .frequency = 1.7 / m_preset.plateSize}).value);
    const f64 pushed = plates.range * along;
    const f64 inland = smooth(-0.02, 0.1, continent);
    return {.warpedX = wx,
            .warpedZ = wz,
            .continent = continent,
            .inland = inland,
            // Island arcs need shallow water: out in the deep, ranges die away.
            .reach = inland + ((1.0 - inland) * kSeaReach * smooth(-0.22, -0.02, continent)),
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
        base = (-m_preset.oceanDepth * (1.0 - smooth(-0.3, -0.03, continent))) -
               (8.0 * smooth(0.0, kCoastShelf, -continent));
    } else {
        base = (2.0 * smooth(0.0, 0.015, continent)) + (kCoastRise * smooth(0.0, 0.07, continent)) +
               (m_preset.inlandHeight * smooth(0.1, kLowland, continent));
    }

    const Fractal ridgeShape{.octaves = m_preset.ridgeOctaves, .frequency = 1.2 / m_preset.rangeWidth};
    const f64 ridge = vxe::noise::ridged2(m_ridges, land.warpedX, land.warpedZ, ridgeShape).value;
    // Most of a range is its broad body, highest along the plate border, so it
    // reads as one long massif with a spine. Crests only ride on top: stronger
    // ridges alone would make lone spikes.
    const f64 mountain = land.strength * land.reach * (0.34 + (ridge * (0.22 + (0.5 * land.strength))));

    // Hills come and go: the rough parts of the land roll, and the rest stays
    // nearly flat, which is what makes plains wide and open. Rifts sink the
    // land along parting plates.
    const Fractal roughShape{.octaves = 3, .frequency = 0.3 / m_preset.hillSize};
    const f64 rough =
        smooth(-0.22, 0.25, vxe::noise::fbm2(m_hills + 1, land.warpedX, land.warpedZ, roughShape).value);
    const Fractal hillShape{.octaves = 5, .frequency = 1.0 / m_preset.hillSize};
    const f64 hills = vxe::noise::fbm2(m_hills, land.warpedX, land.warpedZ, hillShape).value *
                      m_preset.hillHeight * land.inland * (0.1 + (0.9 * rough * rough));
    const f64 riftDrop = land.rift * m_preset.riftDepth * land.inland;

    // Heights past the knee are held back, more the higher they get, so even
    // giants stay under the ceiling. Below the knee nothing changes, and the
    // two parts meet without a crease.
    f64 elevation = base + (mountain * land.tall) + hills - riftDrop;
    if (elevation > m_preset.peakKnee) {
        const f64 over = elevation - m_preset.peakKnee;
        elevation = m_preset.peakKnee + (over / (1.0 + (over / (m_preset.peakCeiling - m_preset.peakKnee))));
    }
    return {.elevation = elevation,
            .continent = continent,
            .mountain = std::min(mountain, 1.0),
            .rift = land.rift,
            .rough = rough};
}

f64 Planet::barrier(f64 x, f64 z) const noexcept {
    const Mass land = mass(x, z, kBarrierOctaves);
    return land.strength * land.reach * land.tall * kMeanCrest;
}

f64 Planet::landMass(f64 x, f64 z) const noexcept {
    return mass(x, z, kBarrierOctaves).continent;
}

} // namespace vr
