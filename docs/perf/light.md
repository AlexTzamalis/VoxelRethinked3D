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
