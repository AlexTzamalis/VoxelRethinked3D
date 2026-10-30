#include "worldgen/planet.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

using namespace vr;
using vxe::f64;
using vxe::u64;
using vxe::usize;

namespace {

struct Survey {
    f64 land = 0.0;
    f64 highest = -1.0e30;
    f64 deepest = 1.0e30;
};

// A sample of the planet on a coarse grid around the origin.
Survey survey(u64 seed) {
    const Planet planet(seed, PlanetPreset{});
    Survey result;
    constexpr int kSteps = 220;
    constexpr f64 kSpacing = 400.0;
    for (int z = -kSteps / 2; z < kSteps / 2; ++z) {
        for (int x = -kSteps / 2; x < kSteps / 2; ++x) {
            const f64 height = planet.sample(x * kSpacing, z * kSpacing).elevation;
            result.land += height > 0.0 ? 1.0 : 0.0;
            result.highest = std::max(result.highest, height);
            result.deepest = std::min(result.deepest, height);
        }
    }
    result.land /= kSteps * kSteps;
    return result;
}

} // namespace

TEST_CASE("A planet is the same for the same seed, whatever the order") {
    const Planet a(5, PlanetPreset{});
    const Planet b(5, PlanetPreset{});
    const Planet other(6, PlanetPreset{});
    usize different = 0;
    for (int i = 0; i < 500; ++i) {
        const f64 x = i * 137.0;
        const f64 z = i * -91.0;
        CHECK(std::bit_cast<u64>(a.sample(x, z).elevation) == std::bit_cast<u64>(b.sample(x, z).elevation));
        different += a.sample(x, z).elevation != other.sample(x, z).elevation ? 1U : 0U;
    }
    CHECK(different > 490);
}

TEST_CASE("Every seed has oceans, land and real mountains") {
    for (const u64 seed : {1ULL, 2ULL, 3ULL, 12345ULL, 987654321ULL, 18446744073709551557ULL}) {
        const Survey world = survey(seed);
        MESSAGE("seed " << seed << ": land " << world.land << ", highest " << world.highest << ", deepest "
                        << world.deepest);
        CHECK(world.land > 0.2);
        CHECK(world.land < 0.85);
        CHECK(world.highest > 350.0);
        CHECK(world.highest < 2400.0);
        CHECK(world.deepest < -100.0);
    }
}

TEST_CASE("The planet has no cuts, only slopes") {
    // A wide scan finds the steepest spot, because a seam shows at only a few
    // places of a world. Walking that spot block by block tells the two apart:
    // on a slope the change spreads over the blocks, a cut stays in one step.
    for (const u64 seed : {7ULL, 12345ULL, 2ULL}) {
        const Planet planet(seed, PlanetPreset{});
        constexpr f64 kStep = 16.0;
        constexpr int kSize = 1400;
        constexpr f64 kStart = -11200.0;
        f64 worst = 0.0;
        f64 worstX = 0.0;
        f64 worstZ = 0.0;
        bool worstAlongX = true;
        std::vector<f64> row(kSize + 1, 0.0);
        for (int z = 0; z < kSize; ++z) {
            const f64 wz = (z * kStep) + kStart;
            f64 left = planet.sample(kStart, wz).elevation;
            for (int x = 1; x <= kSize; ++x) {
                const f64 wx = (x * kStep) + kStart;
                const f64 height = planet.sample(wx, wz).elevation;
                if (std::abs(height - left) > worst) {
                    worst = std::abs(height - left);
                    worstX = wx - kStep;
                    worstZ = wz;
                    worstAlongX = true;
                }
                if (z > 0 && std::abs(height - row[static_cast<usize>(x)]) > worst) {
                    worst = std::abs(height - row[static_cast<usize>(x)]);
                    worstX = wx;
                    worstZ = wz - kStep;
                    worstAlongX = false;
                }
                row[static_cast<usize>(x)] = height;
                left = height;
            }
        }
        f64 worstBlock = 0.0;
        f64 last = planet.sample(worstX, worstZ).elevation;
        for (int i = 1; i <= static_cast<int>(kStep); ++i) {
            const f64 step = i;
            const f64 height =
                planet.sample(worstX + (worstAlongX ? step : 0.0), worstZ + (worstAlongX ? 0.0 : step))
                    .elevation;
            worstBlock = std::max(worstBlock, std::abs(height - last));
            last = height;
        }
        MESSAGE("seed " << seed << ": steepest over 16 blocks " << worst << ", over 1 block there "
                        << worstBlock);
        CHECK(worst < 220.0);
        CHECK(worstBlock < 24.0);
    }
}

TEST_CASE("The planet works the same 30 million blocks out") {
    const Planet planet(9, PlanetPreset{});
    constexpr f64 kFar = 30000000.0;
    f64 worst = 0.0;
    f64 last = planet.sample(kFar, -kFar).elevation;
    for (int i = 1; i < 6000; ++i) {
        const f64 height = planet.sample(kFar + i, -kFar).elevation;
        worst = std::max(worst, std::abs(height - last));
        last = height;
    }
    CHECK(worst < 24.0);
}
