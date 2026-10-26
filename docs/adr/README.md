# Architecture Decision Records

Each file records one architectural decision: the context, what was decided and what follows from it.

A decision is never changed silently. To change one, propose the change with its tradeoffs, get agreement,
then update the record (or add a new one that supersedes it) in the same commit as the change.

| ADR | Decision |
|-----|----------|
| [ADR-001](0001-engine-and-game-separate-targets.md) | Engine plus Game, two separate targets |
| [ADR-002](0002-cpp20-without-modules.md) | C++20, no C++20 modules |
| [ADR-003](0003-thin-rhi-opengl-first.md) | Thin RHI now, OpenGL 4.6 first, Vulkan ready |
| [ADR-004](0004-sdl3-windowing-and-input.md) | Windowing and input through SDL3 |
| [ADR-005](0005-cubic-chunks-16.md) | Cubic chunks of 16x16x16 |
| [ADR-006](0006-integrated-server.md) | Integrated server even in singleplayer |
| [ADR-007](0007-fixed-tick-uncapped-rendering.md) | Fixed simulation tick, uncapped rendering |
| [ADR-008](0008-entt-for-entities-only.md) | ECS via EnTT for entities only |
| [ADR-009](0009-data-driven-content.md) | Data driven content |
| [ADR-010](0010-reversed-z-infinite-far.md) | Reversed Z, 32 bit float depth, infinite far plane |
| [ADR-011](0011-camera-relative-rendering.md) | Camera relative rendering with integer chunk offsets |
| [ADR-012](0012-custom-job-system.md) | Custom job system |
| [ADR-013](0013-imgui-for-tools-custom-game-ui.md) | Dear ImGui for tools, custom UI for the game |
| [ADR-014](0014-toml-settings-and-keybinds.md) | Settings and keybinds persisted as TOML |
| [ADR-015](0015-module-layer-order.md) | Engine modules in a strict layer order, enforced by CMake |
