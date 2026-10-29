# Region file performance

Measured on the development machine (Windows 11, MSVC 19.51, NVMe SSD),
release build, with `build/release/bin/vxe_benchmarks` (nanobench) and the
region tests. Section 21 and ADR-018 describe the format.

## E9.8 Region file IO (2026-10-29)

### Chunk payloads (4 bit terrain surface chunk of the synthetic sample)

| Operation | Time per chunk |
|-----------|----------------|
| encodeChunk (names palette, bit packed indices) | 9.4 us |
| decodeChunk (names to ids, ChunkStorage::assign) | 10.4 us |
| RegionFile::read (sectors, checksum, zstd) | 5.7 us |
| RegionFile::commit, 64 chunks per commit, zstd level 3, two syncs | 43 us (2.7 ms per commit) |

Commits are dominated by the two syncs; batching many chunks per region
into one commit (RegionStore::save) is what keeps saving cheap.

### Sizes

| Data | Bytes |
|------|-------|
| Uniform chunk payload | under 16 |
| Random chunk with 2 states | 532 (uncompressed) |
| Random chunk with 17 states | 2,883 (uncompressed) |
| 32 demo terrain chunks (floor to sky, two regions), payloads | 30,786 |
| The same on disk after zstd, both region files | 25,864 (about 0.8 KB per chunk) |

### Crash safety

- Every byte position of a commit's writes was turned into a simulated crash
  (writes stop part way): the reopened file always held either the whole old
  commit or the whole new one. 408 points kept the old commit; 448 points in
  the second half of the superblock sector (after its 64 used bytes) left
  the new commit complete.
- A writer process committing 64 chunks per round was killed at random
  moments 12 times: the file always opened, always held one whole commit,
  and later runs continued it (commit 977 after the last kill).
