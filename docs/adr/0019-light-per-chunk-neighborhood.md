# ADR-019: Light is computed per chunk from its neighborhood, under open sky from column summaries

- **Status:** Accepted
- **Date:** 2026-10-29

## Context
Section 14 plans block light as add and remove propagation (flood fills that update light incrementally around
an edit) and sky light from the column summary's highest opaque block, with a BFS into overhangs and a bounded
relight below changes. Incremental removal is the classic source of lighting bugs (light that never goes away,
seams where two chunks were updated at different times), and with cubic chunks a chunk's sky depends on chunks
that may not be loaded.

## Decision
- A chunk's sky and block light are computed **from scratch** from its 48 x 48 x 48 neighborhood (the chunk and
  its 26 neighbors) as a job: light reaches at most 15 blocks, so nothing farther can change the result.
  Adding and removing a light source or a roof are the same operation.
- **Sky light** enters each block column at the top of the neighborhood if the column is open: no loaded block
  stands above the neighborhood, unless the loaded chunks of that chunk column do not reach the generator's
  surface estimate and the estimate is above the neighborhood (deep underground, surface not loaded: dark).
  Full light falls straight down through clear blocks without loss; anything that dims it, and all sideways
  and downward spread into overhangs and caves, loses at least one level per block.
- **Streaming** lights a chunk once its 26 neighbors are loaded and meshes it once it and its 26 neighbors are
  lit (loaded within render distance + 2, lit within + 1, meshed within the render distance). Chunks of one
  opaque block, and chunks of air under open sky with no light source in reach, are lit without a job.
- **Relight**: a block edit lights the edited chunk, its 26 neighbors and the chunks below them down to
  `Graphics.Lighting.SkyRelightDepth` chunks again; so does a loaded chunk with blocks above lit chunks.

## Why
- No stale light and no seams by construction; the tests compare every lit block with a flood over the whole
  loaded world.
- A torch updates its chunk within a few milliseconds; a chunk's light job takes about 0.23 ms on one core.

## Consequences
- Blocks more than 32 blocks above a chunk count only as "shaded or not" through the openings; a tall glass roof
  or tree canopy far above shades fully instead of dimming.
- Streaming keeps one more ring of chunks loaded.
