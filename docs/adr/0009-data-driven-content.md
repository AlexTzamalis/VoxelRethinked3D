# ADR-009: Data driven content

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Blocks, biomes, worldgen parameters and texture packs should be tweakable without recompiling, and modding should stay possible later.

## Decision
Define them in TOML or JSON files, not in code.

## Consequences
- Live tweaking and hot reload become natural.
- A path towards modding opens up.
- Loaders need good validation and clear error messages.
