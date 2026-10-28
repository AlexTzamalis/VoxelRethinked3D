# ADR-017: The stat registry lives in core, the overlay in debug

- **Status:** Accepted
- **Date:** 2026-10-27

## Context
Section 6.2 of the plan lists the `StatRegistry` under the `debug` module, which sits at layer 11 (ADR-015).
But the systems that must report stats are mostly below it: jobs (2), io (3), the RHI (7, 8) and the renderer
(9). Under ADR-015 a module may only depend on strictly lower layers, so none of them could register or record a
stat if the registry lived in `debug`.

## Decision
- `vxe::stats::StatRegistry` (registration, lock free recording from any thread, per frame merge, history,
  histograms) lives in `core` (`vxe/core/stats.hpp`), next to logging and memory tracking, which every layer
  also uses.
- Everything that *shows* stats stays in `debug`: overlay pages, graphs, CSV export, the Tracy plot bridge
  (E7.3 and later).
- There is no global registry. Like the other engine services, it is created by the application and passed
  to systems (through `EngineContext` once the overlay is wired up), so tests get independent registries.

## Why
- It is the only placement that lets every layer record stats without breaking the layer rule.
- The registry has no dependencies beyond core itself (time, Result, asserts), so it costs core nothing.

## Consequences
- Section 6.2's table should be read with this ADR: `StatRegistry` is core, `DebugOverlay` and `Console` are
  debug.
