# ADR-005: Cubic chunks of 16x16x16

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Column chunks (16x256x16 style) limit world height and waste work on empty air and solid underground.

## Decision
The world is a sparse 3D grid of 16x16x16 chunks with no column limit.

## Consequences
- Very high vertical limits become possible.
- Empty air and solid underground chunks are skipped cheaply.
- Meshing jobs are small and uniform, and LOD and culling work in 3D.
- Lighting, especially skylight, is harder than in column worlds (see the Risk Register).
