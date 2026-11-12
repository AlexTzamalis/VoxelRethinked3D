# Building, running and debugging

## The short answer

| Question | Answer |
|----------|--------|
| What builds the game? | **Visual Studio 2026's compiler**, always. `build.cmd` finds it for you. |
| Where do I run it? | Any terminal. `.\build.cmd release run` builds and starts it. |
| Can I just start the exe? | Yes: `.\build\release\bin\voxel_rethinked.exe`. That starts the last build; it does not rebuild. |
| Visual Studio 2026 or 2022? | **2026.** Do not open this project in 2022 (see [Why one compiler](#why-one-compiler)). |
| What is CLion for? | Editing, and building if you do the [CLion setup](#clion-setup) once. |
| What is Visual Studio for? | The compiler, and the best debugger for this project. |

## Everyday commands

Run these in the repository folder, from any terminal, including CLion's. Keep the `.\` in front.

| Command | What it does |
|---------|--------------|
| `.\build.cmd` | Builds `release` |
| `.\build.cmd release run` | Builds, then starts the game |
| `.\build.cmd debug` | Builds another preset |
| `.\build.cmd debug test` | Builds, then runs all tests |
| `.\build.cmd release fresh` | Configures from nothing, then builds. Repairs a confused build folder |
| `.\build.cmd release package` | Builds, then makes a copy to share in `out\VoxelRethinked` |

Words after `run` go to the game: `.\build.cmd release run --exec play` goes straight into the world.

A normal rebuild takes seconds to a few minutes. The first build of a preset takes much longer, because vcpkg
builds every library.

## Presets

| Preset | Speed | Use it for |
|--------|-------|------------|
| `release` | Fastest | Playing and measuring. No debug symbols, so it cannot be debugged |
| `debug` | Slow | Stepping through code with a debugger |
| `relwithdebinfo` | Fast | Debugging something that is too slow in `debug`, and profiling with Tracy |
| `asan` | Medium | Hunting memory bugs (AddressSanitizer) |

Each preset has its own folder, `build\<preset>\bin\`. It holds the program and everything it needs:

| Folder | Contents |
|--------|----------|
| `voxel_rethinked.exe` | The game, next to `vxe_tests.exe` and the tools |
| `shaders\`, `assets\`, `textures\`, `sounds\` | Copied from the repository on every build |
Your own files live in **`%APPDATA%\.voxelrethinked\`**, shared by every preset, like `.minecraft`:

| Folder | Contents |
|--------|----------|
| `saves\` | Worlds |
| `config\` | `settings.toml` and `keybinds.toml` |
| `resourcepacks\` | Texture packs you add (the default pack ships in `textures\` next to the program) |
| `shaderpacks\` | Shader packs, later |
| `screenshots\`, `logs\`, `captures\` | Pictures, `latest.log`, overlay recordings |

Read `logs\latest.log` there when something goes wrong at runtime. A program with a `portable.txt` beside it, or
started with `--portable`, keeps all of these next to itself instead. The first start after this change copies
`config\` and `saves\` from next to the program into `%APPDATA%\.voxelrethinked\` once; the old copies stay.

A changed texture reaches the game on the next build. To see edits live, set `Graphics.Textures.HotReload` on and
`Graphics.Textures.Folder` to the repository's `textures` folder.

## First time setup on a new machine

1. Install **Visual Studio 2026** with the workload **Desktop development with C++**. It brings the compiler,
   CMake and Ninja. For the `asan` preset, also tick **C++ AddressSanitizer**.
2. Install vcpkg and tell Windows where it is. This repository expects its own copy, not the one inside Visual
   Studio:
   ```
   git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
   C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   setx VCPKG_ROOT C:\dev\vcpkg
   ```
3. Open a new terminal, because `setx` only reaches new ones, and run `.\build.cmd release run`.

## CLion setup

This machine's CLion needs three changes, once. Until then, its Run button cannot work.

1. **Toolchain.** Open Settings, Build, Execution, Deployment, **Toolchains**. Select **Visual Studio** and set
   Toolset to `C:\Program Files\Microsoft Visual Studio\18\Community`, architecture `amd64`. It points at 2022 now.
2. **CMake profiles.** Open **CMake** on the same page. Delete the old profile **Debug**: it uses MinGW, which
   cannot build this project. Enable the presets you want, usually `debug` and `release`, each with the toolchain
   **Visual Studio**.
3. **Run configuration.** In the Run menu, Edit Configurations, delete **VoxelRethinked3D**, a target that no longer
   exists. Pick `voxel_rethinked` in the top bar, with the profile `release` or `debug`.

After that, the folder `cmake-build-debug` is unused and can be deleted.

If CLion refuses the Visual Studio 2026 toolset, keep CLion as your editor and build with `.\build.cmd` in its
terminal. Nothing else changes.

These steps could not be tried automatically, since they are clicks inside CLion. `build.cmd` is tested.

## Debugging

Use the `debug` preset, or `relwithdebinfo` if `debug` is too slow to reach the bug. `release` has no symbols.

**Visual Studio 2026.** This opens the program without touching the build folders:

```
.\build.cmd debug
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\devenv.exe" .\build\debug\bin\voxel_rethinked.exe
```

Press F5 to start. Open a source file with File, Open to set breakpoints. Build again with `build.cmd` after code
changes; Visual Studio only runs the program here.

**CLion.** After the setup above, pick the `debug` profile and press Debug.

**Inside the game.** These work in every preset:

| Key or option | What it does |
|---------------|--------------|
| F3 | Performance overlay. F3 + Left or Right changes the page |
| Grave (the key under Escape) | Developer console |
| `--exec "command"` | Runs a console command at startup. May repeat |
| `--frames N` | Quits after N frames |

**Profiling.** `relwithdebinfo` includes the Tracy 0.14.1 client. Start the game, then connect with the Tracy
0.14.1 profiler; the two versions must match. `TRACY_NO_EXIT=1` keeps a short run open until Tracy has its data.

## Sharing a build

`.\build.cmd release package` makes `out\VoxelRethinked`. Zip that folder and send it; the game starts from
`voxel_rethinked.exe` inside it. It holds no settings, saves or logs of yours, and it is portable (it has a
`portable.txt`): it keeps its own files in its folder. Delete `portable.txt` for a copy that uses
`%APPDATA%\.voxelrethinked\` like an installed game.

The other PC needs Windows 10 or 11 (64 bit), a GPU with OpenGL 4.5 or newer, and Microsoft's C++ runtime. The
folder includes the matching installer, `vc_redist.x64.exe`. If the game does not start or names `VCRUNTIME140`
or `MSVCP140`, run that once.

## Troubleshooting

| What you see | Why | Fix |
|--------------|-----|-----|
| `build.cmd is not recognized` | The terminal does not search the current folder | Type `.\build.cmd`, in the repository folder |
| `Voxel Rethinked is configured through its presets` | A plain CMake profile was used, for example CLion's old Debug profile | Do the [CLion setup](#clion-setup) |
| `is not able to compile a simple test program`, or `g++` in the error | Same cause, in an older copy of the project | Do the [CLion setup](#clion-setup) |
| `This build folder was made with MSVC tools 14.51, but this tool uses 14.44` | A tool on Visual Studio 2022 opened a 2026 folder | Point that tool at 2026. The folder is fine: `.\build.cmd <preset>` carries on |
| `LNK1168: cannot open voxel_rethinked.exe for writing` | The game is still running | Close it and build again |
| `STL1001` or other strange compiler errors | The folder was left half configured by another compiler | `.\build.cmd <preset> fresh` |
| `vcpkg not found at` | `VCPKG_ROOT` is missing or wrong | Step 2 of the first time setup |
| `cmake is not recognized` | The terminal has no Visual Studio tools | Use `.\build.cmd`, which sets them up itself |
| Old behaviour after changing code | You started the exe without building | `.\build.cmd release run` |
| A folder stays broken after `fresh` | Rare | Delete `build\<preset>\CMakeCache.txt` and `build\<preset>\CMakeFiles`, then run `fresh`. Keep `bin\`, it holds that preset's settings and worlds |

## Why one compiler

Code built by a newer MSVC cannot be linked by an older one. The libraries in `build\<preset>` were built by
Visual Studio 2026, so a tool on 2022 that reconfigures such a folder fails at link time, sometimes after removing
the old program. CMake now stops with a clear message before that happens.

Visual Studio's terminals and tools also replace `VCPKG_ROOT` with a private vcpkg inside Visual Studio. Using it
would install every library a second time, so `cmake/VcpkgToolchain.cmake` reads your own `VCPKG_ROOT` from Windows
instead. Every tool then shares one vcpkg, and CI, which has no such setting, uses its environment variable.
