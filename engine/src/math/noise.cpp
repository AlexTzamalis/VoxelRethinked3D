#include "vxe/math/noise.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace vxe::noise {
namespace {

// The integer below a value, as a plain cast plus a fix for negatives: much
// faster than std::floor without SSE4, and exact.
[[nodiscard]] constexpr i64 floorOf(f64 value) noexcept {
    const auto truncated = static_cast<i64>(value);
    return truncated - (value < static_cast<f64>(truncated) ? 1 : 0);
}

// Quintic fade: its first and second derivatives vanish on cell borders, so
// octaves join without visible creases.
[[nodiscard]] constexpr f64 fade(f64 t) noexcept {
    return t * t * t * (t * ((t * 6.0) - 15.0) + 10.0);
}
[[nodiscard]] constexpr f64 fadeSlope(f64 t) noexcept {
    return 30.0 * t * t * (t * (t - 2.0) + 1.0);
}

// A strong 64 bit mix (the finalizer of MurmurHash3). Multiplies by odd
// constants so neighbors in space end up far apart.
[[nodiscard]] constexpr u64 mix(u64 value) noexcept {
    value ^= value >> 33U;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33U;
    value *= 0xC4CEB9FE1A85EC53ULL;
    return value ^ (value >> 33U);
}

[[nodiscard]] constexpr u64 latticeHash(u64 seed, i64 x, i64 z) noexcept {
    return mix(seed ^ (static_cast<u64>(x) * 0x9E3779B97F4A7C15ULL) ^
               (static_cast<u64>(z) * 0xC2B2AE3D27D4EB4FULL));
}

[[nodiscard]] constexpr u64 latticeHash(u64 seed, i64 x, i64 y, i64 z) noexcept {
    return mix(seed ^ (static_cast<u64>(x) * 0x9E3779B97F4A7C15ULL) ^
               (static_cast<u64>(y) * 0x165667B19E3779F9ULL) ^ (static_cast<u64>(z) * 0xC2B2AE3D27D4EB4FULL));
}

// 16 unit directions, off the axes by half a step, written out so that no
// sin or cos runs: axis aligned gradients show as a grid.
constexpr std::array<std::array<f64, 2>, 16> kDirections2 = {{
    {0.98078528040323, 0.19509032201613},
    {0.83146961230255, 0.55557023301960},
    {0.55557023301960, 0.83146961230255},
    {0.19509032201613, 0.98078528040323},
    {-0.19509032201613, 0.98078528040323},
    {-0.55557023301960, 0.83146961230255},
    {-0.83146961230255, 0.55557023301960},
    {-0.98078528040323, 0.19509032201613},
    {-0.98078528040323, -0.19509032201613},
    {-0.83146961230255, -0.55557023301960},
    {-0.55557023301960, -0.83146961230255},
    {-0.19509032201613, -0.98078528040323},
    {0.19509032201613, -0.98078528040323},
    {0.55557023301960, -0.83146961230255},
    {0.83146961230255, -0.55557023301960},
    {0.98078528040323, -0.19509032201613},
}};

// Ken Perlin's cube edge directions.
constexpr std::array<std::array<f64, 3>, 16> kDirections3 = {{
    {1.0, 1.0, 0.0},
    {-1.0, 1.0, 0.0},
    {1.0, -1.0, 0.0},
    {-1.0, -1.0, 0.0},
    {1.0, 0.0, 1.0},
    {-1.0, 0.0, 1.0},
    {1.0, 0.0, -1.0},
    {-1.0, 0.0, -1.0},
    {0.0, 1.0, 1.0},
    {0.0, -1.0, 1.0},
    {0.0, 1.0, -1.0},
    {0.0, -1.0, -1.0},
    {1.0, 1.0, 0.0},
    {-1.0, 1.0, 0.0},
    {0.0, -1.0, 1.0},
    {0.0, -1.0, -1.0},
}};

// Gradient noise peaks near 0.71 in 2D; this brings it to about 1.
constexpr f64 kScale2 = std::numbers::sqrt2;

// Each octave turns by this rotation (3, 4, 5 triangle: exact fractions).
constexpr f64 kTurnCos = 0.6;
constexpr f64 kTurnSin = 0.8;

} // namespace

u64 hash2(u64 seed, i64 x, i64 z) noexcept {
    return latticeHash(seed, x, z);
}

Sample2 gradient2(u64 seed, f64 x, f64 z) noexcept {
    const i64 ix = floorOf(x);
    const i64 iz = floorOf(z);
    const f64 fx = x - static_cast<f64>(ix);
    const f64 fz = z - static_cast<f64>(iz);
    const auto corner = [&](i64 dx, i64 dz) -> const std::array<f64, 2>& {
        return kDirections2[latticeHash(seed, ix + dx, iz + dz) >> 60U];
    };
    const auto& g00 = corner(0, 0);
    const auto& g10 = corner(1, 0);
    const auto& g01 = corner(0, 1);
    const auto& g11 = corner(1, 1);
    const f64 v00 = (g00[0] * fx) + (g00[1] * fz);
    const f64 v10 = (g10[0] * (fx - 1.0)) + (g10[1] * fz);
    const f64 v01 = (g01[0] * fx) + (g01[1] * (fz - 1.0));
    const f64 v11 = (g11[0] * (fx - 1.0)) + (g11[1] * (fz - 1.0));
    const f64 u = fade(fx);
    const f64 v = fade(fz);
    const f64 du = fadeSlope(fx);
    const f64 dv = fadeSlope(fz);
    // The bilinear blend of the corners, and its derivative by the product rule.
    const f64 a = v00;
    const f64 b = v10 - v00;
    const f64 c = v01 - v00;
    const f64 d = v00 - v10 - v01 + v11;
    const f64 value = a + (b * u) + (c * v) + (d * u * v);
    const f64 gx = g00[0] + (u * (g10[0] - g00[0])) + (v * (g01[0] - g00[0])) +
                   (u * v * (g00[0] - g10[0] - g01[0] + g11[0]));
    const f64 gz = g00[1] + (u * (g10[1] - g00[1])) + (v * (g01[1] - g00[1])) +
                   (u * v * (g00[1] - g10[1] - g01[1] + g11[1]));
    return {.value = value * kScale2,
            .dx = (gx + (du * (b + (d * v)))) * kScale2,
            .dz = (gz + (dv * (c + (d * u)))) * kScale2};
}

f64 gradient3(u64 seed, f64 x, f64 y, f64 z) noexcept {
    const i64 ix = floorOf(x);
    const i64 iy = floorOf(y);
    const i64 iz = floorOf(z);
    const f64 fx = x - static_cast<f64>(ix);
    const f64 fy = y - static_cast<f64>(iy);
    const f64 fz = z - static_cast<f64>(iz);
    const auto corner = [&](i64 dx, i64 dy, i64 dz) {
        const auto& g = kDirections3[latticeHash(seed, ix + dx, iy + dy, iz + dz) >> 60U];
        return (g[0] * (fx - static_cast<f64>(dx))) + (g[1] * (fy - static_cast<f64>(dy))) +
               (g[2] * (fz - static_cast<f64>(dz)));
    };
    const f64 u = fade(fx);
    const f64 v = fade(fy);
    const f64 w = fade(fz);
    const auto lerp = [](f64 a, f64 b, f64 t) { return a + (t * (b - a)); };
    return lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u), lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u), lerp(corner(0, 1, 1), corner(1, 1, 1), u), v),
                w);
}

Sample2 fbm2(u64 seed, f64 x, f64 z, const Fractal& fractal) noexcept {
    Sample2 sum;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    f64 frequency = fractal.frequency;
    // Maps a slope back through the turns so far: turned octaves report their
    // slope in their own axes.
    f64 backXX = 1.0;
    f64 backXZ = 0.0;
    f64 backZX = 0.0;
    f64 backZZ = 1.0;
    f64 px = x;
    f64 pz = z;
    u64 octaveSeed = seed;
    for (u32 octave = 0; octave < fractal.octaves; ++octave) {
        const Sample2 sample = gradient2(octaveSeed, px * frequency, pz * frequency);
        sum.value += sample.value * amplitude;
        const f64 slopeX = sample.dx * amplitude * frequency;
        const f64 slopeZ = sample.dz * amplitude * frequency;
        sum.dx += (backXX * slopeX) + (backXZ * slopeZ);
        sum.dz += (backZX * slopeX) + (backZZ * slopeZ);
        total += amplitude;
        amplitude *= fractal.gain;
        frequency *= fractal.lacunarity;
        octaveSeed = mix(octaveSeed + 1);
        const f64 turnedX = (px * kTurnCos) - (pz * kTurnSin);
        pz = (px * kTurnSin) + (pz * kTurnCos);
        px = turnedX;
        const f64 xx = (backXX * kTurnCos) - (backXZ * kTurnSin);
        const f64 xz = (backXX * kTurnSin) + (backXZ * kTurnCos);
        const f64 zx = (backZX * kTurnCos) - (backZZ * kTurnSin);
        const f64 zz = (backZX * kTurnSin) + (backZZ * kTurnCos);
        backXX = xx;
        backXZ = xz;
        backZX = zx;
        backZZ = zz;
    }
    return {.value = sum.value / total, .dx = sum.dx / total, .dz = sum.dz / total};
}

f64 fbm3(u64 seed, f64 x, f64 y, f64 z, const Fractal& fractal) noexcept {
    f64 sum = 0.0;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    f64 frequency = fractal.frequency;
    u64 octaveSeed = seed;
    for (u32 octave = 0; octave < fractal.octaves; ++octave) {
        sum += gradient3(octaveSeed, x * frequency, y * frequency, z * frequency) * amplitude;
        total += amplitude;
        amplitude *= fractal.gain;
        frequency *= fractal.lacunarity;
        octaveSeed = mix(octaveSeed + 1);
    }
    return sum / total;
}

Sample2 ridged2(u64 seed, f64 x, f64 z, const Fractal& fractal) noexcept {
    Sample2 sum;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    f64 frequency = fractal.frequency;
    f64 weight = 1.0;
    f64 weightX = 0.0;
    f64 weightZ = 0.0;
    u64 octaveSeed = seed;
    for (u32 octave = 0; octave < fractal.octaves; ++octave) {
        const Sample2 sample = gradient2(octaveSeed, x * frequency, z * frequency);
        // 1 - |n| folds the noise into a crest where it crosses zero.
        const f64 sign = sample.value < 0.0 ? -1.0 : 1.0;
        const f64 ridge = 1.0 - (sample.value * sign);
        const f64 ridgeX = -sample.dx * sign * frequency;
        const f64 ridgeZ = -sample.dz * sign * frequency;
        const f64 height = ridge * ridge;
        const f64 heightX = 2.0 * ridge * ridgeX;
        const f64 heightZ = 2.0 * ridge * ridgeZ;
        sum.value += height * amplitude * weight;
        sum.dx += ((heightX * weight) + (height * weightX)) * amplitude;
        sum.dz += ((heightZ * weight) + (height * weightZ)) * amplitude;
        total += amplitude;
        // The next octave is as strong as this crest is high.
        weight = height;
        weightX = heightX;
        weightZ = heightZ;
        amplitude *= fractal.gain;
        frequency *= fractal.lacunarity;
        octaveSeed = mix(octaveSeed + 1);
    }
    return {.value = sum.value / total, .dx = sum.dx / total, .dz = sum.dz / total};
}

WorleyCell worleyPoint(u64 seed, i64 cellX, i64 cellZ, f64 jitter) noexcept {
    const u64 id = latticeHash(seed, cellX, cellZ);
    const f64 offsetX = 0.5 + ((unit(mix(id + 1)) - 0.5) * jitter);
    const f64 offsetZ = 0.5 + ((unit(mix(id + 2)) - 0.5) * jitter);
    return {.x = cellX,
            .z = cellZ,
            .pointX = static_cast<f64>(cellX) + offsetX,
            .pointZ = static_cast<f64>(cellZ) + offsetZ,
            .id = id};
}

WorleyResult worley2(u64 seed, f64 x, f64 z, f64 jitter) noexcept {
    const i64 ix = floorOf(x);
    const i64 iz = floorOf(z);
    WorleyResult result;
    f64 best = 1.0e30;
    f64 next = 1.0e30;
    for (i64 dz = -2; dz <= 2; ++dz) {
        for (i64 dx = -2; dx <= 2; ++dx) {
            WorleyCell cell = worleyPoint(seed, ix + dx, iz + dz, jitter);
            const f64 ax = cell.pointX - x;
            const f64 az = cell.pointZ - z;
            const f64 squared = (ax * ax) + (az * az);
            cell.distance = squared;
            if (squared < best) {
                next = best;
                result.second = result.nearest;
                best = squared;
                result.nearest = cell;
            } else if (squared < next) {
                next = squared;
                result.second = cell;
            }
        }
    }
    result.nearest.distance = std::sqrt(best); // Correctly rounded everywhere, by IEEE 754.
    result.second.distance = std::sqrt(next);
    return result;
}

} // namespace vxe::noise
