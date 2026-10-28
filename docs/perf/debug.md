# Debug tools performance

Measured on the development machine (Windows 11, MSVC 19.51, RTX 4060, driver 610.62). Release builds unless
noted. Section 18.3 of the plan sets the budgets.

## Stat registry (E7.1)

| Case | Budget | Measured |
|------|--------|----------|
| `endFrame` with 500 stats touched every frame, 5 threads recording | 0.05 ms | 0.0073 ms (debug 0.039 ms) |

Source: `engine/tests/core/stats_tests.cpp`, "Closing a frame with 500 stats stays within budget".

## Overlay (E7.3)

| Case | Budget | Measured |
|------|--------|----------|
| `DebugOverlay::update`, overlay hidden | 0 ms | 0.0005 ms |
| `DebugOverlay::update`, compact Overview | 0.2 ms | 0.0043 ms (asan 0.057 ms) |

Source: `engine/tests/debug/overlay_tests.cpp`, "The overlay stays within its frame budget". The time covers
the overlay's own work (keys, CSV, building the ImGui windows), not ImGui's render of all windows.

## Whole frame with the debug UI (vxe_rhi_demo)

| Case | CPU per frame | GPU per frame |
|------|---------------|---------------|
| Cube and triangle, no debug UI (`--no-ui`) | 0.27 ms | 0.03 ms |
| Overlay on the Frame page (`--overlay-page 1`) | 0.45 ms | 0.09 ms |
| ImGui and ImPlot demo windows open (`--imgui-demo`) | 0.40 ms | 0.10 ms |

Uncapped (`--frames N` without `--vsync`), averaged over the run.
