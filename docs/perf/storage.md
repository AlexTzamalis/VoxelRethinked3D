# Chunk storage performance

Measured on the development machine (Windows 11, MSVC 19.51), release build,
with `build/release/bin/vxe_benchmarks` (nanobench). Section 2 sets the
memory targets; Section 9.4 describes the storage.

## E9.2 Chunk storage (2026-10-28)

### Memory (Section 2 targets)

Synthetic terrain sample (engine/tests/voxel/terrain_sample.hpp): 8 x 8 chunk
columns, 12 chunks tall, rolling hills with grass, dirt, sand, stone, water,
gravel, three ores and caves. Checked by the unit test "The terrain sample
meets the Section 2 memory targets".

| Metric | Target | Measured |
|--------|--------|----------|
| All air or all stone chunk | <= 32 bytes | 16 bytes (the object, no heap block) |
| Average mixed chunk, block data | <= 2 KB (2048 bytes) | 2003.4 bytes over 567 mixed chunks |

Index widths in the sample: 3 chunks at 1 bit (520 bytes), 37 at 2 bits
(1032 bytes), 527 at 4 bits (2080 bytes), none at 8 bits. The average meets
the target with little room: almost every surface and underground chunk
needs 5 to 16 states and so 4 bits. Real worldgen (G3) with more block types
may land above 2 KB. If it does, a 3 bit width (21 indices per 64 bit word,
still never crossing a word) would store chunks with 5 to 8 states in 1568
bytes plus the palette; Section 9.4 lists only 1, 2, 4 and 8 bits today, so
that would be a deliberate change to propose then.

### Speed

| Operation | Time |
|-----------|------|
| get, 2 bit or 4 bit palette, random order | 1.9 ns |
| get, raw | 1.6 ns |
| set, 4 bit palette, existing states, random order | 7.1 to 7.3 ns |
| decompressTo, uniform | 88 to 95 ns per chunk |
| decompressTo, 2 or 4 bit palette | 2.3 us per chunk |
| decompressTo, raw | 111 to 114 ns per chunk |
| assign, terrain surface chunk | 6.6 to 7.7 us per chunk (21.6 us before the one pass rewrite) |
| compact, 4 bit chunk with nothing to drop | 4.2 us per chunk |

assign is now about 3% of the 0.25 ms per chunk generation budget.
Decompressing a palette chunk (2.3 us) happens once per mesh or light job;
the meshing budget is 0.06 ms per chunk, so it costs about 4% of that.
