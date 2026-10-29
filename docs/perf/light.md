# Lighting performance

Measured on the development machine (Windows 11, MSVC 19.51, 14 job
workers), release build. Section 14 describes the light engine.

## E10.2 Block light (2026-10-29)

Block light of a chunk is computed from scratch from the chunk and its 26
neighbors (a light source reaches at most 15 blocks): a flood fill over the
48 x 48 x 48 blocks, level by level from 15 down. Chunks with no light
source among their 27 skip the job and get block light 0 on the main thread
(a look at 27 palettes).

| Case | Result |
|------|--------|
| Torch placed in a lit world: its chunk shows light 14 after | 1.8 ms (target: within one frame) |
| Torch removed: dark again after | 1.5 ms |
| Lit blocks matching a flood over the whole loaded world (sources on chunk borders and corners) | all, so no seams at chunk borders |

The flood fill is checked against a brute force relaxation over random
neighborhoods with stone, glass, water (3 levels per block), torches and
glowing opaque blocks.

### Streaming with lighting (demo world, render distance 16, flight at 300 blocks per second)

Lighting needs one more ring of loaded chunks (lit within render distance
+ 1, loaded within + 2).

| Measure | Before (E9.7) | With block light |
|---------|---------------|------------------|
| Playable after entering the world | 255 to 306 ms | 314 to 343 ms |
| Everything in range | 461 to 521 ms | 532 to 549 ms |
| Streaming main thread time per frame while flying | 0.06 to 0.08 ms | 0.08 to 0.10 ms |

The demo terrain has no light sources yet, so every chunk takes the no
source path; the jobs run near lumen blocks once the game places them.

## E10.3 Sky light (2026-10-29)

Sky and block light now come from one light job per chunk (ChunkLighter):
open sky from the column summaries enters at the top of the 48^3
neighborhood, falls through clear blocks and spreads like block light. Chunks
of one opaque block and chunks of open air with no light source in reach are
lit without a job.

### One chunk's light (vxe_benchmarks, terrain surface neighborhood, open sky)

| Version | Time per chunk |
|---------|----------------|
| First version | 485 us |
| Filling the 48^3 box row by row (it was 0.44 of the 0.48 ms) | 251 us |
| A byte per state for opacity and glow instead of the registry's 24 byte info | 228 us |

Stopping the flood at cells too far from the middle chunk to reach it with
their level changed nothing measurable: the fill dominates.

### Streaming (demo world, render distance 16, flight at 300 blocks per second)

| Measure | Block light only (E10.2) | Sky and block light |
|---------|--------------------------|---------------------|
| Playable after entering the world | 314 to 343 ms | 446 to 485 ms |
| Everything in range | 532 to 549 ms | 765 to 843 ms |
| Streaming main thread time per frame | 0.08 to 0.10 ms | 0.14 to 0.18 ms |
| Frames over 1 ms of streaming per 20,000 | 1 to 4 | 0 to 15 |
| 1% low FPS | 410 to 586 | 520 to 633 |

In one flight 99,677 chunks were lit: 58,116 by a job, 41,580 without one,
and no light was redone because a chunk loaded above lit ones. The main
thread spends about 20 us per light job (copying the 27 chunks for the job,
checking and storing the result). The frames over 1 ms grow with how busy the
workers are, like the stalls analysed in docs/perf/streaming.md; copying the
chunks without allocating is the next thing to try if they matter.

The torch test (a closed room, 27 chunks and the chunks below relit with sky
and block light): lit after 4.8 to 5.0 ms, dark again after 4.8 to 4.9 ms on
4 workers.
