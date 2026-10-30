#include <vxe/math/noise.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <thread>
#include <vector>

using namespace vxe;
using namespace vxe::noise;

namespace {

// FNV over the exact bits of a value: one flipped bit changes the result.
void add(u64& hash, f64 value) {
    hash = (hash ^ std::bit_cast<u64>(value)) * 0x100000001B3ULL;
}

// Every function at many positions, near the origin and far out, as one
// number. goldenHash() must give the same on every compiler and machine.
u64 goldenHash() {
    u64 hash = 0xCBF29CE484222325ULL;
    const Fractal fractal{.octaves = 5, .frequency = 0.013};
    for (int i = 0; i < 4000; ++i) {
        const f64 x = (static_cast<f64>(i) * 37.13) - 30000000.0 * (i % 3 == 0 ? 1.0 : 0.0);
        const f64 z = (static_cast<f64>(i) * -91.77) + 30000000.0 * (i % 5 == 0 ? 1.0 : 0.0);
        const f64 y = static_cast<f64>(i % 300) - 150.0;
        const Sample2 a = gradient2(12345, x * 0.01, z * 0.01);
        add(hash, a.value);
        add(hash, a.dx);
        add(hash, a.dz);
        const Sample2 b = fbm2(777, x, z, fractal);
        add(hash, b.value);
        add(hash, b.dx);
        add(hash, b.dz);
        const Sample2 c = ridged2(999, x, z, fractal);
        add(hash, c.value);
        add(hash, c.dx);
        add(hash, gradient3(5, x * 0.02, y * 0.02, z * 0.02));
        add(hash, fbm3(6, x, y, z, fractal));
        const WorleyResult d = worley2(8, x * 0.001, z * 0.001, 0.9);
        add(hash, d.nearest.distance);
        add(hash, d.second.distance);
        add(hash, d.nearest.pointX);
    }
    return hash;
}

} // namespace

TEST_CASE("Noise is the same on every machine: golden hash of exact bits") {
    CHECK(goldenHash() == 0x9BCF5FE55E5C444BULL);
}

TEST_CASE("Noise depends on the seed and on nothing else") {
    CHECK(gradient2(1, 10.3, 20.7).value == gradient2(1, 10.3, 20.7).value);
    CHECK(gradient2(1, 10.3, 20.7).value != gradient2(2, 10.3, 20.7).value);

    // Threads get the same bits, in any order.
    std::vector<u64> hashes(4, 0);
    std::vector<std::thread> threads;
    for (usize i = 0; i < hashes.size(); ++i) {
        threads.emplace_back([&hashes, i] { hashes[i] = goldenHash(); });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const u64 hash : hashes) {
        CHECK(hash == goldenHash());
    }
}

TEST_CASE("Gradient noise stays in range, is 0 on the grid, and has no jumps") {
    f64 low = 1.0;
    f64 high = -1.0;
    f64 largestStep = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const f64 x = static_cast<f64>(i) * 0.0173;
        const f64 z = static_cast<f64>(i) * 0.0091;
        const f64 value = gradient2(3, x, z).value;
        low = std::min(low, value);
        high = std::max(high, value);
        largestStep = std::max(largestStep, std::abs(value - gradient2(3, x + 0.001, z).value));
    }
    CHECK(low > -1.2);
    CHECK(high < 1.2);
    CHECK(low < -0.7);
    CHECK(high > 0.7);
    CHECK(largestStep < 0.01);
    CHECK(gradient2(3, 5.0, -7.0).value == 0.0);
    CHECK(gradient3(3, 5.0, -2.0, 9.0) == 0.0);
}

TEST_CASE("Derivatives match the slope measured from the values") {
    constexpr f64 kStep = 1.0e-4;
    const Fractal fractal{.octaves = 4, .frequency = 0.02};
    for (int i = 0; i < 2000; ++i) {
        const f64 x = (static_cast<f64>(i) * 3.77) - 1000.0;
        const f64 z = (static_cast<f64>(i) * -2.31) + 500.0;
        const auto check = [&](const Sample2& sample, auto&& function) {
            const f64 measuredX =
                (function(x + kStep, z).value - function(x - kStep, z).value) / (2.0 * kStep);
            const f64 measuredZ =
                (function(x, z + kStep).value - function(x, z - kStep).value) / (2.0 * kStep);
            CHECK(sample.dx == doctest::Approx(measuredX).epsilon(1.0e-3).scale(1.0));
            CHECK(sample.dz == doctest::Approx(measuredZ).epsilon(1.0e-3).scale(1.0));
        };
        Sample2 scaled = gradient2(9, x * 0.05, z * 0.05);
        scaled.dx *= 0.05; // Its slope is per noise unit; the check measures per block.
        scaled.dz *= 0.05;
        check(scaled, [](f64 a, f64 b) { return gradient2(9, a * 0.05, b * 0.05); });
        const auto fbm = [&](f64 a, f64 b) { return fbm2(9, a, b, fractal); };
        check(fbm(x, z), fbm);
    }
}

TEST_CASE("Ridged noise is 0 to 1 with sharp crests") {
    const Fractal fractal{.octaves = 5, .frequency = 0.01};
    f64 low = 1.0;
    f64 high = 0.0;
    for (int i = 0; i < 100000; ++i) {
        const f64 value = ridged2(4, static_cast<f64>(i) * 0.7, static_cast<f64>(i) * 0.3, fractal).value;
        low = std::min(low, value);
        high = std::max(high, value);
    }
    CHECK(low >= 0.0);
    CHECK(high <= 1.0);
    CHECK(high > 0.5);
}

TEST_CASE("Noise keeps its detail 30 million blocks from the origin") {
    const Fractal fractal{.octaves = 6, .frequency = 0.005};
    constexpr f64 kFar = 30000000.0;
    f64 largestStep = 0.0;
    usize different = 0;
    f64 last = fbm2(11, kFar, kFar, fractal).value;
    for (int i = 1; i < 5000; ++i) {
        const f64 value = fbm2(11, kFar + (static_cast<f64>(i) * 0.25), kFar, fractal).value;
        largestStep = std::max(largestStep, std::abs(value - last));
        different += value != last ? 1U : 0U;
        last = value;
    }
    CHECK(different == 4999); // No steps from running out of precision.
    CHECK(largestStep < 0.05);
}

TEST_CASE("Worley finds the nearest points in order and is continuous") {
    for (int i = 0; i < 20000; ++i) {
        const f64 x = static_cast<f64>(i) * 0.137;
        const f64 z = static_cast<f64>(i) * -0.291;
        const WorleyResult result = worley2(21, x, z, 1.0);
        CHECK(result.nearest.distance <= result.second.distance);
        CHECK(result.nearest.id != result.second.id);
        // No point of the search area is closer than the nearest.
        const WorleyCell home =
            worleyPoint(21, static_cast<i64>(std::floor(x)), static_cast<i64>(std::floor(z)), 1.0);
        const f64 toHome = std::hypot(home.pointX - x, home.pointZ - z);
        CHECK(result.nearest.distance <= toHome + 1.0e-12);
    }
}

TEST_CASE("Smooth steps are flat at both ends") {
    CHECK(smooth(1.0, 3.0, 0.0) == 0.0);
    CHECK(smooth(1.0, 3.0, 2.0) == 0.5);
    CHECK(smooth(1.0, 3.0, 9.0) == 1.0);
}
