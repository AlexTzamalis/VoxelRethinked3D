# ADR-012: Custom job system, not std::async and not OpenMP

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Generation, meshing and lighting need priorities (near chunks first), cancellation (a chunk left render distance) and profiling. The standard facilities offer none of these.

## Decision
Build a work stealing thread pool with priorities and task dependencies, plus dedicated IO threads.

## Consequences
- Full control over priorities, cancellation and per worker statistics.
- More code to own and test, including ThreadSanitizer runs.
