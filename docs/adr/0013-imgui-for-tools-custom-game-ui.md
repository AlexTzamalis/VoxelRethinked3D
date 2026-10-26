# ADR-013: Dear ImGui for developer tools only, custom UI for the game

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
Developer tools and player facing UI have very different needs. ImGui is ideal for tools but looks like tools.

## Decision
ImGui and ImPlot power the debug overlay, profilers and dev panels. Menus, settings, world creation and the HUD use our own retained mode UI rendered through the RHI.

## Consequences
- Fast iteration on tools.
- The game UI gets styling, animation, controller navigation, scaling and texture pack theming.
- A custom UI engine is real work (see the Risk Register).
