# ADR-004: Windowing and input through SDL3

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
The engine needs windows, a GL context, keyboard, raw mouse, gamepads, high DPI, clipboard and file dialogs on Windows and Linux.

## Decision
Use SDL3 for all of these.

## Consequences
- Modern and actively maintained, with good high DPI and gamepad support.
- SDL3 can also create Vulkan surfaces later, so it does not block ADR-003.
