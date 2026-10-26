# ADR-014: Settings and keybinds persisted as TOML

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Settings and keybinds must survive restarts and be easy for users to read and edit.

## Decision
Store them in `config/settings.toml` and `config/keybinds.toml`, human readable and generated with comments.

## Consequences
- Users can edit them by hand.
- Diffs are readable.
- The loader must preserve unknown keys and report bad values clearly.
