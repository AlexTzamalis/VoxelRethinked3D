# ADR-018: Region files commit copy on write behind two superblocks; chunks carry their own name palettes

- **Status:** Accepted
- **Date:** 2026-10-29

## Context
Section 21 sketches the region file as a header of 4096 entries, sector based allocation, zstd payloads and
"writes go to a temporary sector and the header is updated last". Section 9.3 says saves store "a per region
palette of string names". Building it (E9.8) needed two details the sketch leaves open: how the header itself
survives a crash while it is being written, and where the name palette lives so that a chunk can be read and
written without touching others.

## Decision
- **Two superblocks, copy on write.** Sectors 0 and 1 (512 bytes each) hold two superblocks with a sequence
  number, the location and checksum of the chunk table, and their own checksum. A commit writes new payloads
  and a new chunk table only into sectors the last commit does not use, syncs, writes the next superblock into
  the slot the last commit did not use, and syncs again. Opening takes the valid superblock with the highest
  sequence. The chunk table lists only stored chunks (up to 4096 entries of 24 bytes), not a fixed 4096 entry
  header, so a region with a few edited chunks stays a few KB.
- **Per chunk name palettes.** Each chunk payload starts with the names of the states it uses, then bit packed
  indices. zstd removes most of the repetition between chunks of a region.
- **512 byte sectors**, checksums (XXH3) on every payload and on the chunk table.

## Why
- A torn write can only hit the superblock being written or sectors nobody points to yet, so a crash at any
  byte leaves the last complete commit. The test suite checks this at every byte of the superblock write and
  by killing a writer process mid commit.
- A per region palette would have to be rewritten (and kept consistent) on every commit; per chunk palettes
  keep chunks independent and payloads deterministic, at a small cost after compression.
- Small sectors waste little space: an average compressed terrain chunk is under 1 KB.

## Consequences
- Section 9.3's "per region palette" is read as "per chunk palette, compressed per region file".
- Freed sectors are reused by later commits; files never shrink. Compaction can be added if saves grow.
- A commit costs two syncs; saving batches many chunks per region into one commit (RegionStore::save).
