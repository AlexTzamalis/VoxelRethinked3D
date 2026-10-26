# ADR-006: Integrated server even in singleplayer

- **Status:** Accepted (singleplayer first interpretation awaiting confirmation)
- **Date:** 2026-10-26

## Context
Multiplayer is not built now, but adding it later must not require rewriting the game. The original request mentioned multiplayer, while the surrounding text says the first deliverable is singleplayer.

## Decision
The game always runs an authoritative server and a client. In singleplayer the server runs on its own thread in the same process and talks to the client through an in process message transport.

## Consequences
- Adding networking later means swapping the in process transport for a socket transport.
- All world mutations go through server side handlers, even in singleplayer.
- The plan starts with singleplayer on this architecture. Open Question 1 in the plan asks to confirm this interpretation.
