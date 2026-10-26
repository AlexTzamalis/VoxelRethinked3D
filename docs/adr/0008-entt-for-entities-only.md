# ADR-008: ECS via EnTT for entities only

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
The game has many heterogeneous entities (player, mobs, items, projectiles) and an enormous number of blocks.

## Decision
Entities live in an EnTT registry. Blocks are not entities, they live in specialized chunk storage.

## Consequences
- ECS fits heterogeneous entities well.
- Dense block data gets storage built for it (palettes, uniform chunks).
- EnTT is a game side dependency only.
