# World generation performance

Budget (Section 2): 0.25 ms of terrain generation per 16^3 chunk on one core.
These numbers come from the development machine (16 threads), release preset,
not from the Mid PC of Section 2.

## Noise core (G3.0)

`vxe_benchmarks noise`, release preset, 2026-10-30.

| Function | ns per call |
|----------|-------------|
| gradient2 | 21.6 |
| gradient3 | 26.1 |
| fbm2, 6 octaves | 142 |
| ridged2, 6 octaves | 151 |
| fbm3, 4 octaves | 98 |
| worley2 | 242 |
| One chunk's noise (3 x 25 fbm2 and 75 fbm3) | 17,900 |

One chunk's noise on the coarse grid of Section 12.2 costs 17.9 us, 7% of the
budget, so our own noise is fast enough and FastNoise2 stays unused.

## Determinism

The golden test holds gradient, fractal, ridged and Worley noise to exact bits,
near the origin and 30 million blocks out, as one hash:
`0x9BCF5FE55E5C444B`.

| Build | Hash |
|-------|------|
| MSVC /O2 | same |
| MSVC /Od | same |
| MSVC /O2 /arch:AVX2 | same |
| Clang -O2 | same |
| Clang -O3 -mavx2 -mfma | same |
| GCC -O2 | same |
| GCC -O3 -mavx2 -mfma | same |
| GCC -O3 -mavx2 -mfma, without -ffp-contract=off | different |

The last line is why the root CMakeLists.txt sets `-ffp-contract=off`: GCC fuses
a multiply and an add into one less rounded step when the CPU target allows it,
which changes the world. All compilers ran on Windows; Linux waits for CI.

## Planet (G3.1)

`Planet::sample` takes about 2.5 us. A chunk column needs 25 samples on the
4 block grid, which is about 63 us, and every chunk of a column shares them.

## Climate (G3.2)

A `Climate::Cell` (four grid corners, each looking 4 km upwind) costs about
69 us, measured through vr_seed_map on all threads. A chunk column lies inside
one cell, so a column costs about 130 us with the planet, whatever its height,
and every chunk of the column shares that. A cache of cells can cut it further,
since four columns share one cell.
