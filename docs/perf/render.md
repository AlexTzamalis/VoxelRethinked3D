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
