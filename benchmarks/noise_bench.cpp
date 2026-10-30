#include "benchmarks.hpp"

#include <vxe/math/noise.hpp>

#include <nanobench.h>

// Noise (G3.0). The last line is what one chunk costs in the plan: 5 x 5
// columns of 2D fields on a coarse grid and 5 x 3 x 5 samples of 3D density,
// interpolated between (Section 12.2). The budget is 0.25 ms per chunk.
namespace vxe::bench {

void runNoiseBenchmarks() {
    using namespace vxe::noise;
    ankerl::nanobench::Bench bench;
    bench.title("Noise").minEpochIterations(2000).warmup(10);
    f64 x = 0.0;
    f64 z = 0.0;
    const Fractal six{.octaves = 6, .frequency = 0.002};
    const Fractal four{.octaves = 4, .frequency = 0.01};
    bench.run("gradient2", [&] {
        x += 0.37;
        z += 0.11;
        ankerl::nanobench::doNotOptimizeAway(gradient2(1, x, z));
    });
    bench.run("gradient3", [&] {
        x += 0.37;
        z += 0.11;
        ankerl::nanobench::doNotOptimizeAway(gradient3(1, x, z, x * 0.5));
    });
    bench.run("fbm2, 6 octaves", [&] {
        x += 3.7;
        z += 1.1;
        ankerl::nanobench::doNotOptimizeAway(fbm2(1, x, z, six));
    });
    bench.run("ridged2, 6 octaves", [&] {
        x += 3.7;
        z += 1.1;
        ankerl::nanobench::doNotOptimizeAway(ridged2(1, x, z, six));
    });
    bench.run("fbm3, 4 octaves", [&] {
        x += 3.7;
        z += 1.1;
        ankerl::nanobench::doNotOptimizeAway(fbm3(1, x, z, x * 0.5, four));
    });
    bench.run("worley2", [&] {
        x += 0.37;
        z += 0.11;
        ankerl::nanobench::doNotOptimizeAway(worley2(1, x, z, 0.9));
    });
    bench.minEpochIterations(50).run("one chunk's noise: 3 x 25 fbm2 and 75 fbm3", [&] {
        x += 16.0;
        for (int column = 0; column < 25; ++column) {
            const f64 cx = x + (static_cast<f64>(column % 5) * 4.0);
            const f64 cz = z + (static_cast<f64>(column / 5) * 4.0);
            ankerl::nanobench::doNotOptimizeAway(fbm2(1, cx, cz, six));
            ankerl::nanobench::doNotOptimizeAway(fbm2(2, cx, cz, six));
            ankerl::nanobench::doNotOptimizeAway(fbm2(3, cx, cz, six));
            for (int y = 0; y < 3; ++y) {
                ankerl::nanobench::doNotOptimizeAway(fbm3(4, cx, static_cast<f64>(y) * 8.0, cz, four));
            }
        }
    });
}

} // namespace vxe::bench
