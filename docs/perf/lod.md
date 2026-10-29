# Far LOD performance

Numbers from `vxe_benchmarks` (release) on the development machine (Windows 11,
RTX 4060 laptop, NVMe SSD). Section 13 describes the system.

## E11.1 LOD data structures (2026-10-29)

A test tile of hills (64 x 64 cells, 1.1 spans per cell, 4,272 spans) holds
76.5 KB in memory and 13.9 KB on disk (about 3.2 bytes per span: varint
deltas between neighboring tops, then LZ4).

| Operation | Time per tile |
|-----------|---------------|
| Build with LodTileBuilder | 0.18 ms |
| Encode (varints + LZ4) | 0.23 ms |
| Decode (checksum, LZ4, varints, span checks) | 0.18 ms |
| Cache save (atomic write: temporary file, rename) | 1.84 ms |
| Cache load (file read and decode) | 0.29 ms |

| Clipmap update (6 levels, half extent 4, 304 tiles) | Time |
|------------------------------------------------------|------|
| Camera moved, no level changed (every frame) | 14 ns |
| Level 1 and 2 squares moved (every 256 blocks) | 14.6 us |

Budget: the clipmap check runs every frame on the main thread and costs
nothing measurable; tile building, encoding and cache IO run on workers and
IO threads (E11.2), where a cached tile is ready in about 0.3 ms.

Reach with the defaults (levels 1 to 6, half extent 4): 12,288 to 16,384
blocks. Section 13.3's "High" table reaches about 32,000 with 6 levels; the
LOD settings in E11.2 choose levels and half extent per quality preset (7
levels, or half extent 8, for 32,000 blocks at "Ultra").

## E11.2 LOD manager (2026-10-29)

| Case | Result |
|------|--------|
| LodManager::update, settled, camera still (every frame) | 29 ns |
| All 304 tiles of the default clipmap ready, flat sampler, 14 workers | 9.3 ms after the first update |
| Per tile path on a cache hit / miss | 0.29 ms load on an IO thread / sampler time plus a 1.8 ms save on an IO thread |

Sampling cost depends on the game's column sampler (G3.6); it runs on
workers at low priority and is reported as Voxel.Lod.SampleTime.

## E11.3 LOD mesher and renderer (2026-10-29)

Demo world (voxel_rethinked, release, 1280 x 720, RTX 4060 laptop), camera at
8 110 40, render distance 16, LOD defaults (levels 1 to 6, half extent 4,
304 tiles, 12,288 to 16,384 blocks):

| Measure | Result |
|---------|--------|
| GPU pass Lod | 0.212 to 0.217 ms (Chunks 0.185 ms, whole frame 0.55 to 0.57 ms) |
| FPS standing, LOD on / off | 1169 / 1010 (runs vary more than the difference) |
| FPS flying at 300 blocks per second, LOD on / off | 1079 / 997 |
| Streaming main thread per frame in flight, LOD on / off | 0.176 / 0.203 ms average |

The Lod pass cost is vertex work: the demo hills give one cell height steps
almost everywhere, so tiles carry thousands of thin side quads. Skipping
tiles whose every chunk column is covered by real chunks did not change it
(0.217 to 0.212 ms); the quad count per tile is the lever (E11.6 shows it).

Coverage: a chunk column hides far terrain once every chunk from its lowest
to its highest surface block and the chunk above are meshed. Checked with
Debug.LOD.ShowCoverage (covered fragments magenta) from 110 and 200 blocks
up: with the camera high, the water and beach chunks below the vertical
render distance are not meshed, and far terrain correctly shows there.
