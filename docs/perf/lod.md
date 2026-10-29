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

## E11.4 Premapping (2026-10-29)

Demo world, release, default LOD settings (304 tiles), cache in
saves/demo/lod (2.9 MB for the 304 tiles):

| Case | Premap done | Far terrain ready | Playable |
|------|-------------|-------------------|----------|
| Cold (empty cache): 304 tiles sampled and written during the chunk burst | 596 to 637 ms | 840 to 856 ms | 841 to 857 ms |
| Warm (every tile cached) | 26 to 41 ms (header checks) | 310 to 366 ms | 503 to 516 ms |
| Unit test: LodManager settling on 304 premapped tiles (4 workers, 2 IO threads, no chunk work) | | 35 ms | |

Section 2 targets: cold world creation to playable <= 2.0 s (0.85 s), warm
load <= 1.0 s (0.51 s). The world enters only once every far terrain tile
is shown, so the first frame in the world has the whole horizon.

Meshing tiles for the renderer runs at Normal priority: at Low, the chunk
burst held it back and far terrain was ready only after 750 to 860 ms even
with every tile cached (more LOD job slots or upload budget did not help).

## E11.5 Edit downsampling (2026-10-29)

| Case | Result |
|------|--------|
| One edited chunk column (7 loaded chunks) into all 6 levels, tiles from the cache, written back (unit test, release) | 11.1 to 11.4 ms on an IO thread, nothing on the main thread |

The edit job downsamples the column into its 64 level 1 cells, merges them
with the spans outside the loaded height, and rebuilds the 32 x 32 parent
cells over it in each level up to the clipmap's coarsest, reading and
writing each tile in the cache. Edits run one at a time.

## E11.5b LOD visual parity (2026-10-29)

Far terrain now samples the demo trees and uses the textures' average
colors, and translucent chunks fog themselves. World entry with tree
sampling:

| Case | Premap done | Far terrain ready | Playable |
|------|-------------|-------------------|----------|
| Cold | 640 ms | 1060 ms | 1060 ms |
| Warm | 38 to 47 ms | 432 to 446 ms | 560 to 584 ms |

Both stay inside Section 2 (2.0 s cold, 1.0 s warm). Placing every crown in
levels 1 and 2 and counting tree spots in coarser levels made tiles slower
to sample (cold far terrain 0.85 s before, 1.06 s now).

What caused the seam at the render distance, measured on window captures:
the real water (translucent) was drawn after the fog pass and stayed clear,
about (62, 87, 151) against (83, 115, 166) for the fogged far water next to
it; and the far terrain used each block's base color while the chunks show
textures about 13% darker in linear light, without trees.
