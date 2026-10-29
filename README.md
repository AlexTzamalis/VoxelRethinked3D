# Voxel Rethinked | Minecraft Rethinked

Note! This is a fully educational project, designed and created for learning. I am not an expert, and the software is provided "as is".

---

# Project Details

> This Minecraft clone is different from the standard Minecraft game by being developed in C++ instead of Java.
>
> It uses OpenGL just like Minecraft, but in a much more performant way, especially in the lighting system, the chunk system and the world generation system. The goal is fast world loading, fast chunk generation, and going beyond normal view distance limits with a custom **seed premapped LOD** system that renders many, many chunks plus low level of detail terrain for chunks that are not loaded. Chunks use a **16x16x16** design instead of 16x256x16.

---

# Highlights

| Feature             | Description |
|---------------------|-------------|
| **High FPS**        | GPU driven rendering, tiny 8 byte quads, one draw call for all visible terrain |
| **Cubic Chunks**    | 16x16x16 chunks, very tall worlds, practically infinite X and Z |
| **Far LOD**         | Terrain visible tens of thousands of blocks away, generated straight from the seed |
| **Instant Worlds**  | Multithreaded generation, meshing and streaming for near instant world entry |
| **Texture Packs**   | Drop your own packs into the `textures` folder and stack them |
| **Live Settings**   | Every feature has its own settings category, tweakable while playing |
| **Custom Keybinds** | Fully rebindable controls with conflict detection |
| **Debug Overlay**   | Deep performance overlay: frame times, GPU passes, chunk pipeline, memory |

---

# Architecture

> The project is split into two parts: a reusable **Engine** (`vxe`) built as a library, and the **Game** that runs on top of it. The engine holds the shared code and logic, the game holds the content and rules.

```
+--------------------------------------------------+
|              Voxel Rethinked (Game)              |
|  Menus, Worldgen, Player, Blocks, Server/Client  |
+--------------------------------------------------+
|                   vxe (Engine)                   |
|  Voxel   |  Renderer  |  UI  |  Debug Overlay    |
|  RHI (Render Hardware Interface) -> OpenGL 4.6   |
|  Settings  |  Input & Keybinds  |  Jobs  |  IO   |
|  Core  |  Math  |  Platform (SDL3)               |
+--------------------------------------------------+
```

- **Swappable renderer:** all graphics go through a thin RHI layer. OpenGL 4.6 today, ready for Vulkan later.
- **Integrated server:** even singleplayer runs a local server thread, so multiplayer can be added in the future.
- **Multithreaded:** a custom job system handles generation, lighting, meshing and loading off the main thread.

---

# Technology

<p align="left">
  <a href="https://skillicons.dev">
    <img src="https://skillicons.dev/icons?i=cpp,opengl,cmake&perline=7" />
  </a>
</p>

| Area | Technology |
|------|------------|
| Language | C++20 |
| Graphics | OpenGL 4.6 (through our own RHI) |
| Build | CMake + vcpkg |
| Window & Input | SDL3 |
| Math | glm |
| World Generation | FastNoise2 |
| Entities | EnTT |
| Debug Tools | Dear ImGui + ImPlot, Tracy Profiler |
| Config Files | TOML (toml++) |
| Compression | zstd, LZ4 |
| Testing | doctest, nanobench |

---

# Building

**Prerequisites:** Visual Studio 2022 17.8+ (MSVC) or newer, CMake 3.28+, Ninja and [vcpkg](https://github.com/microsoft/vcpkg) with the `VCPKG_ROOT` environment variable pointing at it. Dependencies are installed automatically from `vcpkg.json` on the first configure.

From a Visual Studio developer shell (x64):

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

| Preset | Use |
|--------|-----|
| `debug` | Day to day development |
| `release` | Optimized build for players |
| `relwithdebinfo` | Optimized with debug info, for profiling |
| `asan` | AddressSanitizer (plus UBSan on Clang and GCC) |
| `tsan` | ThreadSanitizer, Linux only |

Code style is enforced with clang-format (`--target format-check`, or `--target format` to fix) and clang-tidy (`--target tidy`). Architecture decisions live in [docs/adr](docs/adr).

Executables end up in `build/<preset>/bin/`.

**Running:** `build/release/bin/voxel_rethinked.exe` starts the game. Until the menus exist it goes straight into a demo world (a 512 x 128 x 512 block field with hills, beaches, lakes, caves and trees, generated in parallel at startup) in free flight: W, A, S, D, Space and Left Shift fly, the right mouse button looks, Escape pauses, F11 switches fullscreen, F3 shows the performance overlay and the Grave key opens the developer console. `--exec "set Video.FpsLimit 144"` runs a console command at startup and `--frames N` quits after N frames. `vxe_rhi_demo --terrain` shows a field of terrain chunks through the chunk renderer.

**Profiling:** the `relwithdebinfo` preset includes the [Tracy](https://github.com/wolfpld/tracy) 0.14.1 client. Start the game, then connect with the Tracy 0.14.1 profiler (the versions must match). Set `TRACY_NO_EXIT=1` to keep a short run open until the profiler has received its data. In CLion, select the **Visual Studio** toolchain and enable the CMake presets.

---

# Roadmap

- [ ] **Engine:** core, jobs, settings, input, RHI, renderer, voxel systems, lighting, LOD, UI
- [ ] **Game:** main menu, world creation, worldgen, texture packs, player, building
- [ ] **Polish:** quality presets, performance pass
- [ ] **Future:** multiplayer, Vulkan backend

---

# Project Creation

> This Project has been created and maintained via [JetBrains CLion](https://www.jetbrains.com/clion/) Full C++ IDE.
>
> The Project has only been created and maintained on Windows!
>
><p align="left">
  <a href="https://skillicons.dev">
    <img src="https://skillicons.dev/icons?i=visualstudio,clion,cpp,windows,git&perline=7" />
  </a>
</p>

---

# License & Author
This project is released under an **MIT License**. See [LICENSE](https://github.com/AlexTzamalis/TzamalGameEngine/blob/master/LICENSE) for details.
> The Assets that are bundled with the project are created by Alexandros Tzamalis, but can be used elsewhere for free just by providing credits!
