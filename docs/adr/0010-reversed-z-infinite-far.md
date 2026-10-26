# ADR-010: Reversed Z with a floating point depth buffer and an infinite far plane

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Far LOD terrain tens of thousands of blocks away needs depth precision that a standard depth buffer cannot give without z fighting.

## Decision
Use `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`, a `GL_DEPTH_COMPONENT32F` depth buffer and a reversed Z projection with an infinite far plane.

## Consequences
- Near uniform depth precision across enormous distances.
- Depth tests use greater instead of less, and depth clears to 0.
- Maps directly to the native 0 to 1 depth range of Vulkan.
