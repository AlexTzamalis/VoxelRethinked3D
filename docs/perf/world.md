# Voxel world performance

Measured on the development machine (Windows 11, MSVC 19.51), release build,
with `build/release/bin/vxe_benchmarks` (nanobench). Section 9.5 describes
the world and its snapshots.

## E9.3 Voxel world and snapshots (2026-10-29)

### Snapshots (18 x 18 x 18 states, 11.4 KB)

The center is a terrain surface chunk of the synthetic sample with 4 bit
palette indices, the slowest center the sample has.

| Neighbors | Time per snapshot |
|-----------|-------------------|
| Uniform (all stone below, all air level and above) | 2.59 us (target: under 5 us) |
| None loaded (border filled with a given state) | 2.51 us |
| Terrain chunks (mixed palettes) | 5.45 us |
| All air center, uniform neighbors | 1.21 us |

The acceptance test "A snapshot with uniform neighbors takes under 5
microseconds" measures 2.57 us.

Where the time goes: decoding the center into the box costs 1.56 us; the
1736 border cells cost about 1 us for uniform neighbors (one store each) and
about 4 us for mixed ones (one get each). The first version walked the
border through per neighbor span loops and took 5.19 us with uniform
neighbors; a compile time table of border cells per neighbor and the
templated palette decoder brought it to 2.59 us.

Mixed neighbors at 5.45 us are about 9% of the 0.06 ms meshing budget. If
meshing (E9.4) shows the border gather mattering, the face neighbors could
decode their touching slice with whole word reads instead of 256 single
gets.

### Edits

| Operation | Time |
|-----------|------|
| setBlock, interior block of a 4 bit chunk, existing states | 8.5 ns |

This includes the chunk lookup, storage accounting, the version stamp and
the column summary update.

## E10.1 Light storage (2026-10-29)

Chunks now carry light (Section 14.2), and snapshots copy it with the
blocks: 18 x 18 x 18 packed light bytes next to the states (17.1 KB per
snapshot instead of 11.4 KB).

### Memory

| Chunk light | Bytes |
|-------------|-------|
| Uniform (open sky, or dark underground) | 16 (the object, no heap block) |
| Full (any variation) | 16 + 4,096 |

### Snapshots, same benchmark as E9.3

| Neighbors | Before light | With light |
|-----------|--------------|------------|
| Uniform (all stone below, all air level and above) | 2.61 us | 3.02 us (target: under 5 us) |
| Terrain chunks (mixed palettes) | 5.85 us | 5.83 us |
| Terrain chunks, light differing per block everywhere | | 7.24 us |
| No neighbors loaded | 2.51 us | 2.93 us |
| All air center, uniform neighbors | 1.11 us | 1.65 us |

When the chunk and all its neighbors are lit alike, which is nearly every
chunk until light is computed and most chunks after (sky above ground,
dark deep below), the light box is one fill. Copying light chunk by chunk
first made the uniform case 4.16 us.
