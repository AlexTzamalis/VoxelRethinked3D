#pragma once

#include <vxe/core/types.hpp>

namespace vxe::noise {

// Coherent noise for world generation (Section 12, G3.0).
//
// The one promise: the same seed and position give the same bits on every
// machine and compiler, because a world must not depend on who generated it
// (saved chunks sit next to regenerated ones, and multiplayer comes later).
// So this file uses only what IEEE 754 fixes exactly: + - * / and sqrt on
// doubles, plus integer math. No sin, pow or other library functions, whose
// results vary, and no float, which would lose detail far from the origin:
// a double still resolves 2^-28 of a block at 30 million blocks (Section 12.4).
// Golden tests hold every function to exact bits.
//
// Thread safety: all pure functions.

// A value with its slope, which shaping and erosion both need.
struct Sample2 {
    f64 value = 0.0;
    f64 dx = 0.0;
    f64 dz = 0.0;
};

// Gradient noise: smooth, about -1 to 1, 0 on every integer position, with
// exact derivatives.
[[nodiscard]] Sample2 gradient2(u64 seed, f64 x, f64 z) noexcept;
[[nodiscard]] f64 gradient3(u64 seed, f64 x, f64 y, f64 z) noexcept;

// Octaves of gradient noise, each `lacunarity` times finer and `gain` times
// as strong. `frequency` is cycles per block. Each octave is rotated, which
// hides the grid that a plain stack of octaves shows along the axes.
struct Fractal {
    u32 octaves = 4;
    f64 frequency = 0.01;
    f64 lacunarity = 2.0;
    f64 gain = 0.5;
};

// About -1 to 1.
[[nodiscard]] Sample2 fbm2(u64 seed, f64 x, f64 z, const Fractal& fractal) noexcept;
[[nodiscard]] f64 fbm3(u64 seed, f64 x, f64 y, f64 z, const Fractal& fractal) noexcept;

// Sharp crests, 0 to 1: mountain ridges and river lines. Each octave is
// weighted by the one before, so fine detail gathers along the crests and
// valleys stay smooth, as on real mountains.
[[nodiscard]] Sample2 ridged2(u64 seed, f64 x, f64 z, const Fractal& fractal) noexcept;

// One feature point per grid cell, moved by `jitter` (0 to 1, in cells).
// worleyPoint gives the point of a cell, and worley2 the nearest and second
// nearest around a position in cell units.
struct WorleyCell {
    i64 x = 0;
    i64 z = 0;
    f64 pointX = 0.0;
    f64 pointZ = 0.0;
    u64 id = 0; // A hash of the cell and the seed.
    f64 distance = 0.0;
};
struct WorleyResult {
    WorleyCell nearest;
    WorleyCell second;
};
[[nodiscard]] WorleyCell worleyPoint(u64 seed, i64 cellX, i64 cellZ, f64 jitter) noexcept;
[[nodiscard]] WorleyResult worley2(u64 seed, f64 x, f64 z, f64 jitter) noexcept;

// A well mixed 64 bit hash, and the number from 0 up to but not including 1
// made from one.
[[nodiscard]] u64 hash2(u64 seed, i64 x, i64 z) noexcept;
[[nodiscard]] constexpr f64 unit(u64 hash) noexcept {
    return static_cast<f64>(hash >> 11U) * (1.0 / 9007199254740992.0);
}

// Smooth step from 0 at `low` to 1 at `high`, flat at both ends.
[[nodiscard]] constexpr f64 smooth(f64 low, f64 high, f64 value) noexcept {
    const f64 t = (value - low) / (high - low);
    const f64 clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return clamped * clamped * (3.0 - (2.0 * clamped));
}

} // namespace vxe::noise
