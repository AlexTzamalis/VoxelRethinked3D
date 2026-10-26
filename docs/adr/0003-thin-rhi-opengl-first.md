# ADR-003: Thin RHI now, OpenGL 4.6 backend first, Vulkan ready

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Should the renderer talk to OpenGL directly and abstract later, or go through an API agnostic Render Hardware Interface from the start? OpenGL is a global state machine. Once renderer code calls GL directly for months, that state model leaks into every system and a later port becomes a rewrite.

## Decision
Write a thin RHI interface now with one real backend (OpenGL 4.6) and a `Null` backend. The interface is shaped like Vulkan and D3D12 (immutable pipelines, explicit command lists, declared resource usage), because those concepts map well down to OpenGL while OpenGL concepts map badly up to Vulkan. Only wrap what the renderer actually needs.

## Consequences
- Costs roughly one to two weeks up front.
- The `Null` backend enables headless tests and a GPU free dedicated server.
- A Vulkan backend later (Linux performance, macOS through MoltenVK) is an implementation exercise, not a redesign.
- No Vulkan code, no multi queue and no user visible barriers now. The GL backend inserts `glMemoryBarrier` itself from declared usage.
