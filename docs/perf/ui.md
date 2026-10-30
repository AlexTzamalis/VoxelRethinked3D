# UI performance

Measured on the development machine (Windows 11, MSVC 19.51, RTX 4060, driver 610.62), release builds.
Run with `build/release/bin/vxe_benchmarks ui`.

## Layout and drawing (E12.2)

A settings like menu of 162 widgets (a panel, a wrapped description, 40 rows of a label and two buttons),
`benchmarks/ui_bench.cpp`.

| Case | Budget | Measured |
|------|--------|----------|
| Layout of the whole tree (only when the tree, window, scale or font changed) | 0.1 ms | 0.025 ms |
| Drawing the tree into quads, scale 3 (E12.2: every frame the UI shows) | 0.1 ms | 0.032 ms |

## Batching renderer (E12.3)

Same tree. Drawing now goes into a QuadBatch with CPU clipping and atlas regions, and UiContext keeps the
quads until something changes (layout, hover, text, theme, scale or atlas).

| Case | Budget | Measured |
|------|--------|----------|
| Rebuilding the kept quads (only on changes) | 0.1 ms | 0.050 ms |
| A still screen: appending the kept quads (every frame) | 0.01 ms | 0.0022 ms |

The rebuild got slower than E12.2's direct drawing (0.032 ms) because every quad goes through the clip test
and glyph coordinates are mapped into the atlas; it runs on changes only, so a still menu costs 0.0022 ms.

## Settings view (E12.6)

| Case | Budget | Measured |
|------|--------|----------|
| `SettingsView::refresh` with 60 settings (every frame the settings screen shows) | 0.05 ms | 0.0048 ms |

## Main menu start (G4.1)

Release build, from the engine's first log line to the first frame with the menu's text, window 1280 x 720. The
font comes with the first texture pack load, on a job.

| Case | Target | Measured |
|------|--------|----------|
| Engine start to the first menu frame | instant | 347 ms |
| Engine start to the world behind the menu being ready | not waited for | 1.8 s |
