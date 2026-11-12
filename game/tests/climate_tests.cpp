#include "worldgen/climate.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

using namespace vr;
using vxe::f64;
using vxe::u64;
using vxe::usize;

TEST_CASE("Climate is the same for the same seed, in any order") {
    const Planet planet(3, PlanetPreset{});
    const Climate a(3, planet);
    const Climate b(3, planet);
    for (int i = 200; i > 0; --i) {
        const f64 x = i * 91.3;
        const f64 z = i * -47.1;
        const ClimateSample first = a.sample(x, z, 50.0);
        (void)b.sample(z, x, 0.0); // Something else was asked first.
        const ClimateSample second = b.sample(x, z, 50.0);
        CHECK(std::bit_cast<u64>(first.temperature) == std::bit_cast<u64>(second.temperature));
        CHECK(std::bit_cast<u64>(first.rainfall) == std::bit_cast<u64>(second.rainfall));
    }
}

TEST_CASE("A cell gives the same climate as sampling each position alone") {
    const Planet planet(6, PlanetPreset{});
    const Climate climate(6, planet);
    const Climate::Cell cell = climate.cell(-96.0, 320.0);
    for (int i = 0; i < 16; ++i) {
        const f64 x = -96.0 + i;
        const f64 z = 320.0 + (i * 2.0);
        const ClimateSample alone = climate.sample(x, z, 30.0);
        const ClimateSample shared = Climate::sample(cell, x, z, 30.0);
        CHECK(std::bit_cast<u64>(alone.temperature) == std::bit_cast<u64>(shared.temperature));
        CHECK(std::bit_cast<u64>(alone.rainfall) == std::bit_cast<u64>(shared.rainfall));
    }
}

TEST_CASE("Height cools the air, and only above the sea") {
    const Planet planet(4, PlanetPreset{});
    const Climate climate(4, planet);
    const f64 sea = climate.sample(500.0, 900.0, 0.0).temperature;
    CHECK(climate.sample(500.0, 900.0, 300.0).temperature == doctest::Approx(sea - 13.6));
    CHECK(climate.sample(500.0, 900.0, -100.0).temperature == sea);
}

TEST_CASE("Mountains upwind take the rain") {
    usize wetCount = 0;
    usize dryCount = 0;
    f64 wet = 0.0;
    f64 dry = 0.0;
    for (const u64 seed : {1ULL, 2ULL, 3ULL}) {
        const Planet planet(seed, PlanetPreset{});
        const Climate climate(seed, planet);
        for (int z = -14; z < 14; ++z) {
            for (int x = -14; x < 14; ++x) {
                const ClimateSample here = climate.sample(x * 370.0, z * 370.0, 0.0);
                if (here.shadow > 0.6) {
                    dry += here.rainfall;
                    ++dryCount;
                } else if (here.shadow < 0.05) {
                    wet += here.rainfall;
                    ++wetCount;
                }
            }
        }
    }
    REQUIRE(wetCount > 100);
    REQUIRE(dryCount > 100);
    MESSAGE("Open land rain " << wet / static_cast<f64>(wetCount) << ", in shadows "
                              << dry / static_cast<f64>(dryCount));
    CHECK(dry / static_cast<f64>(dryCount) < (wet / static_cast<f64>(wetCount)) - 0.2);
}

TEST_CASE("Climate changes gently: nothing jumps between grid cells") {
    const Planet planet(5, PlanetPreset{});
    const Climate climate(5, planet);
    f64 heat = 0.0;
    f64 rain = 0.0;
    ClimateSample last = climate.sample(1000.0, 2000.0, 0.0);
    for (int i = 1; i < 3000; ++i) {
        const ClimateSample here = climate.sample(1000.0 + (i * 8.0), 2000.0, 0.0);
        heat = std::max(heat, std::abs(here.temperature - last.temperature));
        rain = std::max(rain, std::abs(here.rainfall - last.rainfall));
        last = here;
    }
    CHECK(heat < 0.5);
    CHECK(rain < 0.05);
}

TEST_CASE("The world holds every biome, and none takes over") {
    std::array<usize, kBiomeCount> counts{};
    usize land = 0;
    usize total = 0;
    for (const u64 seed : {1ULL, 2ULL, 3ULL, 12345ULL, 987654321ULL, 7ULL}) {
        const Planet planet(seed, PlanetPreset{});
        const Climate climate(seed, planet);
        for (int z = -28; z < 28; ++z) {
            for (int x = -28; x < 28; ++x) {
                const f64 wx = x * 480.0;
                const f64 wz = z * 480.0;
                const PlanetSample ground = planet.sample(wx, wz);
                const Biome biome = Climate::biome(ground, climate.sample(wx, wz, ground.elevation));
                ++counts[static_cast<usize>(biome)];
                land += ground.elevation >= 0.0 ? 1U : 0U;
                ++total;
            }
        }
    }
    for (usize i = 0; i < kBiomeCount; ++i) {
        MESSAGE(kBiomeNames[i] << ": " << 100.0 * static_cast<f64>(counts[i]) / static_cast<f64>(total)
                               << "%");
        CHECK(counts[i] > 0);
        if (i > static_cast<usize>(Biome::FrozenOcean) && i != static_cast<usize>(Biome::LukewarmOcean)) {
            CHECK(static_cast<f64>(counts[i]) < static_cast<f64>(land) * 0.4);
        }
    }
}
