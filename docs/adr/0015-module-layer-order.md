# ADR-015: Engine modules in a strict layer order, enforced by CMake

- **Status:** Accepted
- **Date:** 2026-10-27

## Context
The layer diagram (Section 6.1 of the plan) says lower layers never depend on higher layers, but a diagram
cannot stop a stray `#include` or `target_link_libraries`. Every engine header also lives under one include
root (`engine/include/vxe/`), so linking rules alone do not stop a module from including headers of a module
it does not depend on. The OpenGL rule (only `rhi_gl` may touch GL, ADR-003) needs the same kind of guard.

## Decision
Every engine module is created with `vxe_add_module()` (`cmake/VxeModule.cmake`) and gets a fixed layer from
one table:

| Layer | Modules |
|-------|---------|
| 0 | core |
| 1 | math |
| 2 | jobs |
| 3 | io |
| 4 | platform, net |
| 5 | config |
| 6 | input |
| 7 | rhi |
| 8 | rhi_gl, rhi_null |
| 9 | render |
| 10 | ui |
| 11 | debug |
| 12 | voxel |
| 13 | app (Application, EngineContext and the main loop, which ties every other module together) |

- A module may only depend on modules in **strictly lower** layers. Modules on the same layer cannot depend
  on each other (for example `rhi_gl` and `rhi_null`).
- `net` may additionally only depend on `core`, `jobs` and `io`, so a headless server can use it.
- Graphics API libraries (glad, OpenGL) may only be linked by `rhi_gl`, and only `PRIVATE`.
- No engine module may link a game target.
- A source scan rejects `#include <vxe/X/...>` for modules outside the dependency closure, and glad or GL
  headers outside `rhi_gl`. It runs at configure time and again at build time when a file changes. The game
  is scanned for GL headers too.
- Script tests in `cmake/tests/` prove that every rule fires with the right message.

## Consequences
- Wrong dependencies fail at configure time with a message that names both modules and their layers.
- The table turns side by side boxes of the diagram into a total order. Where the diagram is ambiguous
  (config below input, render below ui below debug) the table decides. Changing the order means editing the
  table and this ADR together.
- The RHI device factory cannot live in `rhi`, because it creates backends from layer 8. Decided in E6.1:
  each backend exposes its own create function (`createNullDevice`, later `createGlDevice`), and the
  factory that picks one from a setting lives in `render` (layer 9), which may depend on both.
- `Application` (Section 6.5 sketches it in `vxe/core`) lives in the `app` module at the top, because it
  needs the platform, jobs and IO modules and core may depend on nothing. Added 2026-10-27 in E4.5.
