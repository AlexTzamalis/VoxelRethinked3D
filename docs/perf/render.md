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
