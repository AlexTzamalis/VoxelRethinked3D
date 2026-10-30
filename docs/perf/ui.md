# UI performance

Measured on the development machine (Windows 11, MSVC 19.51, RTX 4060, driver 610.62), release builds.
Run with `build/release/bin/vxe_benchmarks ui`.

## Layout and drawing (E12.2)

A settings like menu of 162 widgets (a panel, a wrapped description, 40 rows of a label and two buttons),
`benchmarks/ui_bench.cpp`.

| Case | Budget | Measured |
|------|--------|----------|
| Layout of the whole tree (only when the tree, window, scale or font changed) | 0.1 ms | 0.025 ms |
| Drawing the tree into quads, scale 3 (every frame the UI shows) | 0.1 ms | 0.032 ms |

E12.3 keeps the quads of an unchanged tree, so still frames skip the drawing cost.
