# ADR-001: Engine plus Game, two separate targets

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
The project needs reusable voxel technology and a game built on it. A future dedicated server should be able to reuse the engine without a window or GPU.

## Decision
Build `vxe` (Voxel eXperience Engine) as static libraries and `voxel_rethinked` as an executable that links them. The engine never includes anything from `game/`.

## Consequences
- Clear boundaries keep engine code reusable and testable without the game.
- A dedicated server later is just another executable linking the same engine.
- The engine holds generic voxel technology (storage, meshing, streaming, LOD, RHI, UI, settings, input). The game holds content and rules (blocks, terrain, menus, player behaviour).
