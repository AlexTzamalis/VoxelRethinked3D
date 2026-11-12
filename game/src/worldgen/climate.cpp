#include "climate.hpp"

#include <vxe/math/noise.hpp>

#include <algorithm>
#include <cmath>

namespace vr {
namespace {

using vxe::f64;
using vxe::i64;
using vxe::u64;
using vxe::noise::Fractal;
using vxe::noise::smooth;

constexpr f64 kMeanWarmth = 11.0;  // Degrees at sea level, on average.
constexpr f64 kWarmthSwing = 46.0; // Reach of the noise around it, about 18 either way.
constexpr int kWindSteps = 6;      // Samples upwind.

u64 stream(u64 seed, u64 salt) noexcept {
    return vxe::noise::hash2(seed, static_cast<i64>(salt), 0xC11A);
}

} // namespace

Climate::Climate(u64 seed, const Planet& planet) noexcept
    : m_planet(&planet)
    , m_warmth(stream(seed, 1))
    , m_detail(stream(seed, 2))
    , m_rain(stream(seed, 3))
    , m_windX(stream(seed, 4))
    , m_windZ(stream(seed, 5))
    , m_variety(stream(seed, 6)) {}

Climate::Field Climate::field(f64 x, f64 z) const noexcept {
    // Climate follows the size of the world: a preset with small continents
    // gets its climates close together, and low ranges still cool and shade.
    const PlanetPreset& preset = m_planet->preset();
    const f64 reach = preset.continentSize / 70000.0;
    const f64 tall = preset.rangeHeight / 620.0;
    const Fractal warmthShape{.octaves = 3, .frequency = 1.0 / (60000.0 * reach)};
    const f64 temperature =
        kMeanWarmth + (vxe::noise::fbm2(m_warmth, x, z, warmthShape).value * kWarmthSwing) +
        (vxe::noise::fbm2(m_detail, x, z, {.octaves = 2, .frequency = 1.0 / (9000.0 * reach)}).value * 3.0);

    // The wind turns slowly across the world. The steady part keeps it from
    // ever dying, which would have no direction.
    const Fractal windShape{.octaves = 2, .frequency = 1.0 / (90000.0 * reach)};
    const f64 rawX = (vxe::noise::fbm2(m_windX, x, z, windShape).value * 2.0) + 0.45;
    const f64 rawZ = vxe::noise::fbm2(m_windZ, x, z, windShape).value * 2.0;
    const f64 speed = std::sqrt((rawX * rawX) + (rawZ * rawZ));
    const f64 windX = speed > 1.0e-9 ? rawX / speed : 1.0;
    const f64 windZ = speed > 1.0e-9 ? rawZ / speed : 0.0;

    // Mountains upwind have taken the rain. Near ones count most, and only the
    // tallest matters, since rain that fell does not fall twice.
    f64 blocked = 0.0;
    for (int step = 1; step <= kWindSteps; ++step) {
        const f64 distance = static_cast<f64>(step) * 1400.0 * reach;
        const f64 fade = 1.0 - (static_cast<f64>(step - 1) / static_cast<f64>(kWindSteps));
        blocked = std::max(blocked, m_planet->barrier(x - (windX * distance), z - (windZ * distance)) * fade);
    }
    const f64 shadow = smooth(60.0 * tall, 520.0 * tall, blocked);
    // Air forced up the near side of a range rains out there.
    const f64 ahead = 1200.0 * reach;
    const f64 facing =
        smooth(50.0 * tall, 420.0 * tall, m_planet->barrier(x + (windX * ahead), z + (windZ * ahead)));

    const Fractal rainShape{.octaves = 5, .frequency = 1.0 / (16000.0 * reach), .gain = 0.55};
    const f64 wet = 0.47 + (vxe::noise::fbm2(m_rain, x, z, rainShape).value * 0.85);
    const f64 coast = 1.0 - smooth(0.0, 0.3, m_planet->landMass(x, z));
    // Hot air sinks and dries the land, as around the tropics on Earth, which
    // is what gives a world its great deserts.
    const f64 baked = smooth(19.0, 31.0, temperature) * 0.24;
    const f64 rainfall = wet + (coast * 0.14) + (facing * (1.0 - shadow) * 0.22) - (shadow * 0.55) - baked;
    const f64 variety =
        smooth(-0.3, 0.3,
               vxe::noise::fbm2(m_variety, x, z, {.octaves = 3, .frequency = 1.0 / (10000.0 * reach)}).value);
    return {.temperature = temperature,
            .rainfall = std::clamp(rainfall, 0.0, 1.0),
            .shadow = shadow,
            .variety = variety};
}

Climate::Cell Climate::cell(f64 x, f64 z) const noexcept {
    const auto size = static_cast<f64>(kGridSize);
    const f64 cornerX = std::floor(x / size) * size;
    const f64 cornerZ = std::floor(z / size) * size;
    return {.coolPerBlock = coolPerBlock(),
            .cornerX = cornerX,
            .cornerZ = cornerZ,
            .a = field(cornerX, cornerZ),
            .b = field(cornerX + size, cornerZ),
            .c = field(cornerX, cornerZ + size),
            .d = field(cornerX + size, cornerZ + size)};
}

ClimateSample Climate::sample(const Cell& cell, f64 x, f64 z, f64 elevation) noexcept {
    const auto size = static_cast<f64>(kGridSize);
    const f64 tx = (x - cell.cornerX) / size;
    const f64 tz = (z - cell.cornerZ) / size;
    const auto blend = [&](f64 Field::* part) {
        const f64 top = cell.a.*part + ((cell.b.*part - cell.a.*part) * tx);
        const f64 bottom = cell.c.*part + ((cell.d.*part - cell.c.*part) * tx);
        return top + ((bottom - top) * tz);
    };
    // Height cools from sea level only: the sea floor is not colder than the sea.
    return {.temperature = blend(&Field::temperature) - (std::max(elevation, 0.0) * cell.coolPerBlock),
            .rainfall = blend(&Field::rainfall),
            .shadow = blend(&Field::shadow),
            .variety = blend(&Field::variety)};
}

f64 Climate::coolPerBlock() const noexcept {
    return 13.6 / m_planet->preset().rangeHeight;
}

ClimateSample Climate::fromField(const Field& field, f64 elevation, f64 coolPerBlock) noexcept {
    return {.temperature = field.temperature - (std::max(elevation, 0.0) * coolPerBlock),
            .rainfall = field.rainfall,
            .shadow = field.shadow,
            .variety = field.variety};
}

ClimateSample Climate::sample(f64 x, f64 z, f64 elevation) const noexcept {
    return sample(cell(x, z), x, z, elevation);
}

Biome Climate::biome(const PlanetSample& ground, const ClimateSample& climate) noexcept {
    const f64 heat = climate.temperature;
    const f64 rain = climate.rainfall;
    const f64 variety = climate.variety;
    if (ground.elevation < 0.0) {
        if (heat < -1.0) {
            return Biome::FrozenOcean;
        }
        if (ground.elevation < -38.0) {
            return Biome::DeepOcean;
        }
        if (heat > 23.0) {
            return Biome::WarmOcean;
        }
        return heat > 15.0 ? Biome::LukewarmOcean : Biome::Ocean;
    }
    // Shores belong to the coast. Low ground inland is just low ground, or a
    // sand or stone plain would appear in the middle of a country.
    if (ground.elevation < 3.0 && ground.continent < 0.018) {
        if (ground.mountain > 0.09) {
            return Biome::StonyShore;
        }
        return heat < 0.0 ? Biome::SnowyBeach : Biome::Beach;
    }
    // Up a mountain the climate gets colder by itself, so these bands follow
    // the height: wooded slopes or meadows, bare rock, then snow.
    if (ground.mountain > 0.12) {
        // Any peak tall enough wears snow, whatever the land below it is like.
        if ((heat < 0.0 && ground.elevation > 100.0) || ground.elevation > 300.0) {
            return Biome::SnowyPeaks;
        }
        if (ground.elevation > 170.0 && (heat < 4.0 || rain < 0.22)) {
            return Biome::RockyPeaks;
        }
        return rain > 0.42 && variety > 0.35 ? Biome::WoodedSlopes : Biome::Meadow;
    }
    if (ground.elevation < 18.0 && rain > 0.62 && heat > 8.0 && ground.rough < 0.5) {
        return Biome::Swamp;
    }
    if (heat < 1.5) {
        return rain < 0.42 ? Biome::SnowyPlains : Biome::SnowyTaiga;
    }
    if (heat < 8.0) {
        if (rain < 0.3) {
            return Biome::Shrubland;
        }
        return variety > 0.5 || rain > 0.62 ? Biome::DeepSpruceForest : Biome::Taiga;
    }
    if (heat < 21.0) {
        if (rain < 0.28) {
            return Biome::Shrubland;
        }
        if (rain < 0.48) {
            return variety > 0.72 ? Biome::FlowerPlains : Biome::Plains;
        }
        if (rain < 0.66) {
            if (variety < 0.4) {
                return Biome::Forest;
            }
            // The cooler end of the leafy woods turns red and gold.
            if (heat < 15.0 && variety > 0.55) {
                return Biome::MapleForest;
            }
            return variety < 0.75 ? Biome::BirchForest : Biome::FlowerForest;
        }
        return variety < 0.6 ? Biome::DarkForest : Biome::PaleForest;
    }
    if (rain < 0.26) {
        return ground.mountain > 0.03 || ground.rift > 0.2 || ground.rough > 0.7 ? Biome::Badlands
                                                                                 : Biome::Desert;
    }
    return rain < 0.56 ? Biome::Savanna : Biome::Jungle;
}

} // namespace vr
