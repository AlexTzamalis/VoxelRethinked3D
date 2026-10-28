# Meshing performance

Measured on the development machine (Windows 11, MSVC 19.51), release build,
with `build/release/bin/vxe_benchmarks` (nanobench). Section 10 describes the
binary greedy mesher; Section 2 sets the budget of 0.06 ms per chunk on one
core.

## E9.4 Binary greedy mesher (2026-10-29)

### Terrain sample (acceptance)

All 768 chunks of the synthetic terrain sample (8 x 8 chunk columns, 12
chunks tall: hills, water, caves, ores), meshed from snapshots with real
neighbors, best of 20 passes:

| Metric | Value |
|--------|-------|
| Average time per chunk | 0.0224 ms (budget 0.06 ms) |
| Chunks with quads | 554 of 768 |
| Quads per non empty chunk | 225.2 |
| Quad bytes per non empty chunk | 1802 bytes (8 bytes per quad; the GPU target is 3 KB per chunk) |

### Single chunks

| Case | Time per chunk |
|------|----------------|
| All air | 1.9 us |
| All stone, stone neighbors | 4.1 us |
| Terrain surface chunk (4 bit palette) | 26.3 us |
| Random 50% stone | 381 us |
| Checkerboard, worst case (12,288 quads, nothing merges) | 482 us |

The worst cases cost about 30 to 40 ns per quad; they do not happen in
generated terrain, and a chunk that was edited into one is meshed once.

### What mattered

The first working version averaged 0.0455 ms over the sample (65 us for a
surface chunk, 838 us for the checkerboard). Two changes brought it down:

- Merge keys (texture, AO, light, state) read the 8 AO neighbors as shifted
  bits of the opaque row masks with fixed row and bit steps per face,
  instead of building 3D positions, and the hot loops index arrays without
  bounds checks (std::array::at stays checked in release builds).
- Rows of one state (most rows: air above ground, stone below) build their
  masks from one lookup instead of 18.
