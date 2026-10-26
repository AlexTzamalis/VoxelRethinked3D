# ADR-002: C++20, no C++20 modules (for now)

- **Status:** Accepted
- **Date:** 2026-10-26

## Context
C++20 modules promise faster builds, but support across MSVC, Clang, GCC, CMake, vcpkg, IDEs and third party libraries is still uneven.

## Decision
Use the C++20 language and library with headers and precompiled headers. No `import` modules. CMake's module dependency scanning is turned off.

## Consequences
- Headers are predictable on every compiler and tool.
- `std::expected` is C++23, so the engine ships its own `vxe::Result<T, E>`.
- Revisit when all three compilers and CMake handle modules with third party code without friction.
