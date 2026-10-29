# Renderer performance

Measured on the development machine (Windows 11, MSVC 19.51, RTX 4060, driver 610.62). Release builds unless
noted. Section 8 of the plan describes the renderer.

## E8.1 Frame graph lite (2026-10-28)

Budget: Section 8.1 sets no number. The graph runs every frame on the main
thread, so it must cost a small fraction of the 0.5 ms idle main thread budget
(Section 2) and allocate nothing per frame.

| Case | Result |
|------|--------|
| `execute()` with 11 passes, 10 transient targets, Null device (test "Executing the frame graph does not allocate and stays within budget") | 0.0010 ms per frame, 0 allocations in 1000 frames |
| Same, debug preset | 0.0105 ms per frame |
| Aliasing in that ping pong chain of 10 targets | 2 GPU textures instead of 10 |

vxe_rhi_demo on the graph (3 passes, 1280x720, overlay on the GPU page,
vsync on):

| Pass | GPU time (average of 60 frames) |
|------|---------------------------------|
| Cube (offscreen color and depth, 1280x720) | 0.061 ms |
| Composite (blit to the window and triangle) | 0.033 ms |
| DebugUi (overlay text and windows) | 0.011 ms |
| All passes | 0.105 ms (whole frame timer: 0.108 ms) |

Per pass timings come from timestamp query pairs read four frames late
(rhi::GpuFrameTimer), so they never stall the CPU.

## E8.2 Camera, frame constants, debug draw (2026-10-28)

Budget: debug drawing is a developer tool, so it only has to stay cheap enough
to leave on while profiling, and allocate nothing per frame.

| Case | Result |
|------|--------|
| Recording the test scene (grid of 33 x 33 lines plus a box) 100 times, Null device | 0 allocations |
| vxe_rhi_demo DebugDraw pass: grid of 193 x 193 lines plus chunk borders (about 440 lines), 1280x720 | 0.133 ms GPU (average of 60) |
| Same frame, other passes | Cube 0.013 ms, Composite 0.025 ms, DebugUi 0.007 ms |

Precision (the E8.2 acceptance): the same scene with the camera at the origin
and at (10,000,000, 64, -10,000,000) gives bit identical line vertices and
bit identical pixels on the RTX 4060 (test "Debug lines render the same far
from the origin"). The first frame after creating the line pipelines differs
in 12 pixels where lines converge near the horizon, whatever the camera
position; every later frame matches exactly.

## E8.3 Sky and fog (2026-10-28)

Budget: one fullscreen pass with a few dozen instructions per pixel; it must
stay well under 0.1 ms at 1080p on the reference GPU class.

| Case | Result |
|------|--------|
| vxe_rhi_demo Sky pass, 1280x720, noon, fog on | 0.042 ms GPU (average of 60), max 0.043 ms |
| Same frame, other passes | Cube 0.015 ms, DebugDraw 0.038 ms, Composite 0.028 ms, DebugUi 0.009 ms |

The pass reads the depth once per pixel with texelFetch and computes the
height fog in closed form, so its cost is flat per pixel and does not grow
with view distance.

## E8.4 Post and present (2026-10-28)

Acceptance: the frame limiter is accurate within 2%.

| Case | Result |
|------|--------|
| FramePacer at 60 FPS, frames doing 0 to 2 ms of varying work (test "The frame limiter is accurate within 2 percent") | 60.008 FPS (+0.01%) |
| Same at 144 FPS | 144.001 FPS (+0.001%) |
| Same at 240 FPS | 240.000 FPS |
| vxe_rhi_demo with set Video.FpsLimit 144, 300 frames including startup | 6.957 ms per frame = 143.7 FPS (-0.2%) |

Post processing: one fullscreen pass (tonemapping, FXAA with up to 9 bilinear
taps from an RGBA16F scene, sRGB encoding) at 1280x720 made the demo's
Composite pass 0.098 ms (average of 60, max 0.103 ms) instead of 0.028 ms with
the plain blit it replaced: 0.07 ms for post processing. Section 8.1 sets no
number. 1080p has 2.25 times the pixels, so roughly 0.16 ms is expected there
(not measured yet); FXAA off skips most of the taps.

Tonemapping check on the GPU: a scene value of 4.0 gives 231 with Reinhard
(4 / 5 = 0.8 linear, sRGB encoded) and 188 with no curve at exposure 0.125,
matching the CPU reference (render::tonemap).

## E9.5 Chunk renderer (2026-10-29)

Scene: `vxe_rhi_demo --terrain`, a field of 32 x 8 x 32 chunks of the
synthetic terrain sample (8192 chunks, 6592 with quads), release build,
1280x720, RTX 4060, camera above the field looking across it.

| Metric | Value |
|--------|-------|
| Quads resident | 1,541,235 |
| GPU bytes per meshed chunk (quads plus 16 bytes of metadata) | 1886 (target 3 KB, Section 2) |
| Vertex arena in use | 11.8 MiB |
| Frames to upload the field (4 MB per frame budget) | 3 |
| GPU frame time | 0.64 ms |
| CPU frame time, debug UI off | 0.76 ms (about 1300 FPS) |
| Draw calls per frame, debug UI off | 4: an opaque and a translucent indirect multi draw (the sample has no cutout blocks), the sky and post processing |
| Invalid commands, driver errors | 0, 0 |

Every chunk is drawn: there is no culling yet (E9.6). Generation of the field
takes 1.4 s and meshing 0.25 s, both on the main thread at startup; the
streaming manager (E9.7) moves that onto workers.

The GPU test "Chunks render with vertex pulling, the same far from the
origin" draws a stone chunk 20 blocks away at the origin and 10 million
blocks away (identical images), and every face of a block head on with and
without the AO diagonal flip, checking both triangles of each quad.

## E9.6 GPU culling (2026-10-29)

Scene: `voxel_rethinked` demo world at the spawn view (4735 chunks, 2.1
million quads, 31,237 non empty face groups), release build, 1280x720, RTX
4060, vsync off, 6000 frames; the numbers are the last 1000 frames, logged
at exit. Two runs per row.

| Culling | Face groups drawn | GPU frame | Average FPS | 1% low |
|---------|-------------------|-----------|-------------|--------|
| None (Visibility pass off) | 31,237 | 0.83 ms | 1117 / 1125 | 420 / 578 |
| CPU: frustum, direction, caves | 7,087 | 0.37 to 0.39 ms | 1598 / 1993 | 1004 / 991 |
| GPU: frustum only | 13,957 | | 1705 / 1713 | 769 / 816 |
| GPU: frustum, direction, caves | 7,087 | 0.42 to 0.43 ms | 1982 / 1992 | 904 / 890 |
| GPU: all, with Hi-Z occlusion | 4,038 | 0.31 ms | 1789 / 2127 | 1000 / 1336 |

Of the 31,237 groups, the frustum removes 17,280, face direction 6,699,
caves 171 (the spawn view is above ground) and occlusion 3,049. GPU time
drops from 0.83 to 0.31 ms (63% less). At around 2000 FPS the frame is
limited by the CPU side (about 0.5 ms), so occlusion shows up as GPU
headroom and a better 1% low more than as average FPS. GPU and CPU culling
agree exactly on every count (GPU test with occlusion off).

The CPU path tests each chunk's box once for all its face groups; before
that it gained nothing over no culling (1090 FPS), because testing every
group on the main thread ate what the GPU saved.

Cave search (render::CaveCuller), release, a 65 x 16 x 65 chunk world with
27,040 known chunks, 34,380 cells reached:

| Case | Time |
|------|------|
| Camera moved into another chunk (grid reused) | 0.46 ms |
| Chunks changed (grid rebuilt) | about 1.1 ms |
| First version (revisiting every cell from every side, divisions per step) | 2.3 ms |

Only partly open chunks are searched once per entry side: a path never
reverses along an axis, so a fully open chunk expands the same exits
whichever side it is entered from, and a solid chunk expands nothing.

Depth pyramid at 1920 x 1080: 11 levels on a 1024 x 1024 base, 5.3 MiB.
The demo world's surface view measured the whole pyramid build plus the
culling dispatch well under the GPU savings (0.42 ms without Hi-Z versus
0.31 ms with it, the build included).

Debug.FreezeCulling keeps the culling camera and the pyramid; the frozen
view is drawn as yellow frustum lines, checked with a window capture.
