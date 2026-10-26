# ADR-011: Camera relative rendering with integer chunk offsets

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Single precision float positions jitter visibly far from the world origin, and the world is practically infinite in X and Z.

## Decision
CPU world positions are `int32` chunk coordinates plus a local offset (`double` for entities). The GPU only receives positions relative to the chunk of the camera, computed with integer math.

## Consequences
- No floating point jitter even millions of blocks from the origin.
- Shaders combine an integer chunk offset with a small float fraction.
- Coordinate spaces must be kept strictly apart with strong types (Section 9.1 of the plan).
