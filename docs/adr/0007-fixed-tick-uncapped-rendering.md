# ADR-007: Fixed simulation tick, uncapped rendering

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Simulation needs to be deterministic and stable, while rendering should run as fast as the hardware allows.

## Decision
The server simulates at a fixed tick rate (default 30 ticks per second, configurable). The client renders uncapped and interpolates entity state between ticks.

## Consequences
- Deterministic simulation and stable physics.
- Frame rate is independent of simulation cost.
- The client needs interpolation and prediction code.
