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

constexpr f64 kMeanWarmth = 15.0;  // Degrees at sea level, on average.
constexpr f64 kWarmthSwing = 46.0; // Reach of the noise around it, about 18 either way.
constexpr f64 kCoolPerBlock = 0.022;
constexpr int kWindSteps = 9;    // Samples upwind.
constexpr f64 kWindStep = 450.0; // Blocks between them, so wind sees about 4 km.
constexpr f64 kFacingReach = 500.0;

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
    , m_windZ(stream(seed, 5)) {}

Climate::Field Climate::field(f64 x, f64 z) const noexcept {
    const Fractal warmthShape{.octaves = 3, .frequency = 1.0 / 26000.0};
    const f64 temperature =
        kMeanWarmth + (vxe::noise::fbm2(m_warmth, x, z, warmthShape).value * kWarmthSwing) +
        (vxe::noise::fbm2(m_detail, x, z, {.octaves = 2, .frequency = 1.0 / 3000.0}).value * 3.0);

    // The wind turns slowly across the world. The steady part keeps it from
    // ever dying, which would have no direction.
    const Fractal windShape{.octaves = 2, .frequency = 1.0 / 32000.0};
    const f64 rawX = (vxe::noise::fbm2(m_windX, x, z, windShape).value * 2.0) + 0.45;
    const f64 rawZ = vxe::noise::fbm2(m_windZ, x, z, windShape).value * 2.0;
    const f64 speed = std::sqrt((rawX * rawX) + (rawZ * rawZ));
    const f64 windX = speed > 1.0e-9 ? rawX / speed : 1.0;
    const f64 windZ = speed > 1.0e-9 ? rawZ / speed : 0.0;

    // Mountains upwind have taken the rain. Near ones count most, and only the
    // tallest matters, since rain that fell does not fall twice.
    f64 blocked = 0.0;
    for (int step = 1; step <= kWindSteps; ++step) {
        const f64 distance = static_cast<f64>(step) * kWindStep;
        const f64 fade = 1.0 - (static_cast<f64>(step - 1) / static_cast<f64>(kWindSteps));
        blocked = std::max(blocked, m_planet->barrier(x - (windX * distance), z - (windZ * distance)) * fade);
    }
    const f64 shadow = smooth(60.0, 520.0, blocked);
    // Air forced up the near side of a range rains out there.
    const f64 facing =
        smooth(50.0, 420.0, m_planet->barrier(x + (windX * kFacingReach), z + (windZ * kFacingReach)));

    const Fractal rainShape{.octaves = 5, .frequency = 1.0 / 7000.0, .gain = 0.55};
    const f64 wet = 0.47 + (vxe::noise::fbm2(m_rain, x, z, rainShape).value * 0.85);
    const f64 coast = 1.0 - smooth(0.0, 0.3, m_planet->landMass(x, z));
    // Hot air sinks and dries the land, as around the tropics on Earth, which
    // is what gives a world its great deserts.
    const f64 baked = smooth(19.0, 31.0, temperature) * 0.24;
    const f64 rainfall = wet + (coast * 0.14) + (facing * (1.0 - shadow) * 0.22) - (shadow * 0.55) - baked;
    return {.temperature = temperature, .rainfall = std::clamp(rainfall, 0.0, 1.0), .shadow = shadow};
}

Climate::Cell Climate::cell(f64 x, f64 z) const noexcept {
    const auto size = static_cast<f64>(kGridSize);
    const f64 cornerX = std::floor(x / size) * size;
    const f64 cornerZ = std::floor(z / size) * size;
    return {.cornerX = cornerX,
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
    return {.temperature = blend(&Field::temperature) - (std::max(elevation, 0.0) * kCoolPerBlock),
            .rainfall = blend(&Field::rainfall),
            .shadow = blend(&Field::shadow)};
}

ClimateSample Climate::sample(f64 x, f64 z, f64 elevation) const noexcept {
    return sample(cell(x, z), x, z, elevation);
}

Biome Climate::biome(const PlanetSample& ground, const ClimateSample& climate) noexcept {
    const f64 heat = climate.temperature;
    const f64 rain = climate.rainfall;
    if (ground.elevation < 0.0) {
        if (heat < -1.0) {
            return Biome::FrozenOcean;
        }
        if (ground.elevation < -70.0) {
            return Biome::DeepOcean;
        }
        return heat > 23.0 ? Biome::WarmOcean : Biome::Ocean;
    }
    if (ground.elevation < 3.0 && ground.mountain < 0.1) {
        return Biome::Beach;
    }
    if (ground.mountain > 0.22) {
        if (heat < -3.0) {
            return Biome::SnowyPeaks;
        }
        return heat < 7.0 || rain < 0.3 ? Biome::RockyPeaks : Biome::AlpineMeadow;
    }
    if (ground.elevation < 16.0 && rain > 0.7 && heat > 6.0) {
        return Biome::Swamp;
    }
    if (heat < -1.0) {
        return rain < 0.42 ? Biome::Tundra : Biome::SnowyTaiga;
    }
    if (heat < 8.0) {
        return rain < 0.34 ? Biome::Steppe : Biome::Taiga;
    }
    if (heat < 21.0) {
        if (rain < 0.3) {
            return Biome::Steppe;
        }
        if (rain < 0.5) {
            return Biome::Plains;
        }
        return rain < 0.68 ? Biome::Forest : Biome::DenseForest;
    }
    if (rain < 0.26) {
        return ground.mountain > 0.03 || ground.rift > 0.2 ? Biome::Badlands : Biome::Desert;
    }
    return rain < 0.56 ? Biome::Savanna : Biome::Jungle;
}

} // namespace vr
