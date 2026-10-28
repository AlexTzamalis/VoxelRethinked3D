# Changelog

All notable changes to Voxel Rethinked. Each entry matches exactly one commit.

## [#047] 2026-10-28, E8.1 Frame graph lite
### Added
- The render module (layer 9) with render::FrameGraph (Section 8.1). Passes are added in execution order through a PassBuilder and declare transient textures (sized relative to the output or fixed), color, depth and window attachments with load and store ops, sampled and storage texture reads and writes, and how they use imported buffers.
- compile() rejects reads before writes, sampling a texture the pass renders into, loading undefined contents, raster passes without attachments, window plus texture attachments, attachments on compute or transfer passes, wrong depth formats and bad or duplicate names, with messages naming the pass and resource.
- Transient textures are created on the first execute and recreated when the output size changes. Textures with equal format, size and usage share one GPU texture when their passes do not overlap (Debug.FrameGraph.Aliasing, live).
- Barriers after shader storage writes, planned once at compile, including writes late in one frame read early in the next (Hi-Z to culling).
- Every pass has a live toggle Debug.Passes.<name> (except passes created with canDisable = false). A disabled pass still clears the attachments it would have cleared.
- Every pass runs in a debug group and is timed on the GPU with a timestamp pair read four frames late (stat Gpu.Pass.<name>). Stats Render.FrameGraph.TransientMemory, Render.FrameGraph.Barriers and Render.FrameGraph.PassesRun.
- The overlay's GPU page lists every Gpu.Pass stat with its average and maximum over 60 frames and the total.
- operator|= for RHI flag enums.
- vxe_rhi_demo renders through a frame graph with the passes Cube, Composite and DebugUi; the frame graph owns its offscreen targets.
- Tests: every validation rule, pass order and render pass contents, resizing, aliasing on and off, barrier planning across frames, disabled passes, and drawing plus GPU timing on the OpenGL device (VXE_GUI_TESTS=1).
### Performance
- execute() with 11 passes: 0.0010 ms per frame in release (0.0105 ms in debug), 0 allocations in 1000 frames. Demo passes on the RTX 4060: Cube 0.061 ms, Composite 0.033 ms, DebugUi 0.011 ms.
### Docs
- docs/perf/render.md with the frame graph measurements.

## [#046] 2026-10-28, Check frame limiter accuracy only on real machines
### Fixed
- The frame limiter tests from #045 failed on the Windows CI runners (156.9 and 189.5 FPS measured for a 200 FPS cap, 136.4 for 144): shared CI machines stall threads for milliseconds. On CI the tests now check that the cap is never exceeded and that "set Video.FpsLimit 144" clearly lowers the rate; the 5% accuracy checks run on real machines (local: 200.0 and 144.0). The limiter itself is unchanged.
- test::timingIsReliable() (false when the CI variable is set) and test::environmentFlag() in the test helpers.

## [#045] 2026-10-28, E7.4 Developer console UI
### Added
- debug::DeveloperConsole: an ImGui console over the console commands (set, get, reset, find, list, help and any command a system registers). The Grave key opens and closes it; while open it pushes a "Console" input context that blocks gameplay input. Enter runs the line, Up and Down walk the history (repeats are kept once), Tab completes the word being typed (a single match is filled in, several are listed and their common start is filled in). Input, output and errors have their own colors.
- Settings Debug.Console.MaxLines and Debug.Console.HistorySize, the action debug.console.toggle and the stat Debug.Console.Commands.
- platform::FrameLimiter for Video.FpsLimit (Section 8.5): keeps a fixed frame schedule and waits with SDL's precise delay (sleep, then a short spin), so the average rate matches the cap closely without burning a core.
- vxe_rhi_demo registers Video.FpsLimit, reads it every frame and runs the console; --console opens it at startup and --exec COMMAND runs console commands at startup.
- Tests: command echo, errors and stats, history walking, completion, the Grave key and context blocking, drawing on the Null device, the limiter at 200 FPS (measured 200.0), and the acceptance test: "set Video.FpsLimit 144" typed into the console changes a running loop from 500 to 144 FPS (measured 499.9 and 144.0).

## [#044] 2026-10-28, Make the allocation counter test immune to allocation elision
### Fixed
- "Every global operator new is counted" failed in the Linux Clang release and relwithdebinfo CI jobs: C++ allows optimizers to remove the allocations of new expressions and std::allocator, and optimized Clang did that for the test's small objects, so fewer than 3 allocations were counted. The test now calls ::operator new directly, which cannot be elided, and checks for exactly 3. The counter itself was correct.

## [#043] 2026-10-28, E7.3 Overlay framework and first pages
### Added
- debug::DebugOverlay, the F3 performance overlay (Section 18.2): a compact Overview (FPS, 1% and 0.1% lows, CPU and GPU time, tracked memory, allocations and draw calls per frame, lines added by other systems), refreshed at Debug.Overlay.UpdateRate so it stays readable.
- Pages as dockable ImGui windows: Frame (ImPlot CPU and GPU graph of the last 1000 frames, 50/90/99/99.9 percentiles, histogram, hitch log of frames over Debug.Overlay.HitchFactor times the median), GPU (capabilities, per frame draw calls and state changes, memory, invalid commands, driver messages), Memory (per tag current and peak, allocations per frame), Input (context stack, active actions, recent raw input), Settings diff (changed settings with reset buttons) and All stats (filterable, with a CSV capture button). Systems add pages with addPage().
- Keys in an always active Debug input context: F3 toggles on release unless it was held for F3+Left or F3+Right, which switch pages.
- Settings Debug.Overlay.Visible, Page, Opacity, Scale, UpdateRate, CsvSeconds and HitchFactor.
- Stats Frame.CpuTime and Frame.GpuTime (1000 frames of history), Memory.Allocations, Gpu.DrawCalls, Gpu.StateChanges, Gpu.BufferMemory, Gpu.TextureMemory and Debug.Overlay.DrawTime.
- debug::StatCsvRecorder: records every stat once per frame for Debug.Overlay.CsvSeconds into captures/stats-<date>-<time>.csv, written on a background thread.
- DebugUiDesc::layoutFile: ImGui window layouts are loaded at creation and saved at destruction, never during frames.
- vxe_rhi_demo runs the overlay (--overlay-page N opens it on a page); the ImGui and ImPlot demos moved behind --imgui-demo.
- Tests: frame time summaries, F3 and chord handling through simulated keys, every page drawing on the Null device with valid commands, changed settings, frame and GPU stats, a CSV capture end to end, the overlay's frame budget, and two ImPlot line regression tests (vertex UVs on the Null device, drawn pixels on the GPU).
### Fixed
- ImPlot lines were nearly invisible: ImGui 1.93 stores baked line texture coordinates as (u0, u1, v, 1/thickness) while ImPlot 1.0 reads (u0, v0, u1, v1), so lines sampled random atlas texels. The debug UI now builds its atlas without baked lines, and both libraries draw anti aliased lines as geometry.
### Performance
- Overlay update: 0.0005 ms hidden, 0.0043 ms in compact mode (budgets 0 and 0.2 ms). Recorded in docs/perf/debug.md together with the E7.1 numbers.

## [#042] 2026-10-28, E7.3 checkpoint: global allocation counting
### Added
- vxe::memory::globalAllocationCount(): every global operator new since startup, for the overlay's Memory page (allocations per frame). Core replaces the global operator new and delete, forwarding to malloc and free with one relaxed atomic increment. Sanitizer builds keep their own allocator and report the count as unavailable.
- InputSystem::contextName, for the overlay's Input page.
### Changed
- clang-tidy excludes Visual Studio, vcpkg and third party headers (MSVC's "VC/Tools" folder matched the tools/ header filter).

## [#041] 2026-10-28, E7.2 ImGui and ImPlot integration
### Added
- Dear ImGui 1.93 (docking) and ImPlot 1.0 from vcpkg, in a new debug module (layer 11) that links them PUBLIC for developer panels.
- debug::ImGuiRenderer: draws ImGui through the RHI instead of ImGui's OpenGL backend. Vertices are pulled from a storage buffer (debug/imgui.vert reads 5 words per vertex), indices are 16 bit, and both live in a 3 frame ring of persistently mapped buffers guarded by fences. Implements ImGui's 1.92+ texture protocol (create, update, destroy, Alpha8 expanded to RGBA8). Scissors are flipped to the RHI's lower left origin.
- debug::DebugUi: owns the ImGui and ImPlot contexts, feeds them platform events (scancodes mapped to ImGuiKey, mouse, wheel, text, focus), and routes input: while ImGui wants the mouse or keyboard it pushes a "Debug UI" input context that blocks gameplay contexts below it.
- Window::setTextInputEnabled, since SDL only delivers typed text while text input is on; the UI asks for it only while a text field is active.
- vxe_rhi_demo shows the ImGui demo window, the ImPlot demo and a CPU, GPU and draw call readout on top of the cube (--no-ui turns them off).
- Tests: key mapping, the demo windows over 8 frames on the Null device with no invalid commands and no leaks, input capture and release, typed text reaching a text field, and a GPU test that reads back exactly 160,000 drawn pixels for a 400x400 window with 0 driver errors.
### Changed
- Keyboard navigation is off in the debug UI: with it, ImGui claims the keyboard whenever a debug window has focus, which would block gameplay keys while the overlay is open.
### Performance
- vxe_rhi_demo in release with both demo windows open: 0.40 ms CPU and 0.10 ms GPU per frame (0.27 ms and 0.03 ms without the UI).

## [#040] 2026-10-28, E7.1 Stat registry
### Added
- vxe::stats::StatRegistry in core (vxe/core/stats.hpp): counters, gauges, timers and histograms registered by dotted name (the prefix is the overlay category), up to 2048 stats.
- Lock free recording from any thread: each thread writes its own slots and marks them in a dirty bitset; endFrame merges only touched slots, so its cost follows the stats actually used. Gauges and histograms use shared atomics.
- Per frame values with preallocated history rings (no allocation per frame), average, maximum, per second rates from the frame durations, and quarter octave histograms with percentiles.
- ScopedStatTimer, and the registry's own Debug.Stats.MergeTime timer.
- VXE_SANITIZER_ACTIVE compile definition in sanitizer builds, so timing assertions can skip them.
- Tests: registration rules, counters summed across 8 threads, a stress test that closes frames while 4 threads record (no count lost, 290 repeated runs), gauges, timers, histograms, history, rates, and the 500 stat budget.
### Performance
- endFrame with 500 stats touched every frame and 5 recording threads: 0.0073 ms in release on the dev machine (budget 0.05 ms), 0.039 ms in debug.
### Docs
- ADR-017: the stat registry lives in core so lower layers can record stats; overlay pages stay in debug.

## [#039] 2026-10-28, Fix a racy job system shutdown test
### Fixed
- The test "Destroying the job system runs every queued job first" read the counter again after incrementing it, so with two workers both increments could land before either read and no job scheduled the extra one (500 instead of 501, seen once under TSan in CI). It now uses the value fetch_add returns, which is unique per job. The job system itself was correct.

## [#038] 2026-10-27, E6.6 RHI hardening
### Added
- rhi::DeferredReleaseQueue: destroyed objects wait until the GPU has completed the frame that may still use them, the rule a Vulkan backend needs, shared by every backend.
- The OpenGL backend inserts a fence per presented frame, polls them without waiting in beginFrame and present, and deletes deferred buffers, textures, samplers, shaders and programs once their frame completed. Handles become invalid immediately.
- Device::waitIdle (drains the GPU and releases everything pending) and DeviceStats::pendingDestroys.
- Device::liveResources, the leak report: every object not destroyed yet, by name. Both devices log it when destroyed; the demo fails its scripted run when it is not empty.
- Automatic names ("unnamed buffer 3") for objects created without a debug name, used for GL labels and the leak report; samplers keep their names; the engine VAO and cached framebuffers are labeled.
- Tests for the release queue, automatic names, the Null leak listing, and GL deferral (pending until frames complete or waitIdle). Every GL test now ends with an empty leak report.

## [#037] 2026-10-27, E6.5 GL command execution, sync, queries, swapchain
### Added
- gl::CommandReplayer: full command list replay for the OpenGL backend. Render passes to the window or to cached framebuffer objects, clears (float, integer, depth, depth stencil), LoadOp and StoreOp DontCare invalidation, viewport and scissor, uniform, storage and texture binds through the set * 16 + slot rule, index buffers on one engine VAO, draw, drawIndexed, multi draw indirect and indirect count (core 4.6 or ARB), compute dispatch, buffer copies, barriers mapped to glMemoryBarrier bits, debug groups and timestamps.
- Every command is checked before it reaches OpenGL (live handles, usage flags, offset alignment, ranges, binding limits, pass rules); invalid ones are counted and skipped. Unclosed passes and debug groups are closed and counted at the end of a list.
- Reversed Z clip space: glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) at device start (ADR-010).
- Timestamp queries that never wait (createTimestampQuery, timestampNanoseconds) and rhi::GpuFrameTimer, a 4 frame ring that reads results 3 frames later.
- Device::readTexture and TextureUsage::TransferSrc for tests and screenshots; capabilities report uniform and storage offset alignment; DeviceStats counts draw calls and dispatches.
- shader_source helpers folderShaderReader and createShaderFromFile, shared by the checker, the demo and the tests.
- Engine shaders debug/cube (an indexed 8 corner cube with derivative based flat shading) and debug/blit (fullscreen triangle copy).
- tools/rhi_demo (vxe_rhi_demo): a spinning cube rendered offscreen with depth, copied to the window, with the triangle on top and GPU and CPU frame times in the title. --frames N runs a scripted check. Measured on an RTX 4060, release: 0.27 ms CPU and 0.05 ms GPU per frame, 0 invalid commands, 0 driver errors.
- GPU tests with pixel readback: reversed Z depth rejection, clipping below depth 0, identical images from direct, indirect and indirect count draws (count 0 draws nothing), compute fill with barriers and copies, the GPU frame timer, and exactly 8 invalid commands skipped without driver errors. Null device tests for timestamps, the timer and texture readback.
### Changed
- The OpenGL backend is split into gl_resources, gl_commands and gl_device.
- readBuffer copies into a host side readback buffer and waits on a fence, instead of reading the GPU buffer directly, which made NVIDIA move the source buffer to system memory.
- Shaders for the Null backend get VXE_OPENGL, since it follows OpenGL's binding model.
### Fixed
- The Linux Clang asan job failed to link vxe_shader_check because UBSan's vptr check needs RTTI; glslang is now built with its rtti feature.
### Docs
- ADR-016: RHI coordinate conventions (lower left origin, bottom row first, 0 to 1 depth, window shows values as written).

## [#036] 2026-10-27, E6.4 OpenGL shaders and pipelines
### Added
- rhi/shader_source: the GLSL preprocessor shared by runtime and build. It resolves #include "path" relative to the shader root, honors #pragma once, rejects cycles, unsafe paths and #version in files with file:line messages, writes #version 460 core plus backend and stage defines, and emits #line directives so every line maps back to its file.
- annotateShaderLog rewrites NVIDIA, AMD, Mesa and glslang compiler logs into file:line messages.
- Engine shaders in engine/shaders: include/bindings.glsl with the VXE_BINDING macros (Section 7.3) and the debug triangle and fill buffer shaders for the RHI smoke tests.
- tools/shader_check (vxe_shader_check): runs the engine preprocessor and glslang 16.4 on each shader and prints clickable absolute file:line errors. cmake/Shaders.cmake checks every .vert, .frag and .comp file as part of the build, so an invalid shader fails the build.
- CTest cases that expect the checker to fail on a broken shader, a broken include file and a missing include, each with the right file and line.
- OpenGL shaders and pipelines: compile errors returned with file names (for example include/colors.glsl:3), program linking with shaders detached so they can be destroyed early, and immutable pipelines holding all fixed function state.
- gl::StateCache: redundant state filtering for program, polygon mode, culling, front face, depth clamp, depth test, write and compare, and per attachment blending. Measured on an RTX 4060: 29 state calls for the first pass over 8 binds, 13 for the same binds once the state is known.
- Pipeline binds replayed by submit (the rest of command replay follows in E6.5); repeated binds of the bound pipeline are skipped. DeviceStats counts pipeline binds, skipped binds and state changes, on both backends.
- GPU tests (VXE_GUI_TESTS=1) for engine shaders, pipelines, state filtering, readable compile and link errors, and stage checks.
### Changed
- ShaderDesc carries the file names behind each source number, and the debug callback no longer repeats compiler messages that already come back as errors.
- Raster, depth and blend state are comparable; the alpha blend comment now describes straight alpha correctly.

## [#035] 2026-10-27, E6.3 OpenGL device, buffers, textures, samplers
### Added
- engine/third_party/glad: the glad2 2.0.8 loader generated for OpenGL 4.6 core plus bindless textures, indirect parameters, gl_spirv, anisotropic filtering, KHR_debug and the NVX and ATI memory info extensions, with the exact regeneration command. Built as C++ and linked only by rhi_gl, privately.
- vxe_rhi_gl module (layer 8) with createGlDevice: loads GL through SDL, requires OpenGL 4.5, detects indirect count draws, bindless textures, SPIR-V, timestamps, texture limits, anisotropy and video memory, and logs them at startup.
- KHR_debug output with synchronous delivery by default; driver errors and warnings go to the new rhi log category and are counted in DeviceStats. A startup marker message proves the callback works before debug output is reported as active.
- Direct State Access resources: immutable buffers (CpuWrite buffers persistently and coherently mapped), 2D and 2D array textures with per mip and per layer uploads for every RHI format, samplers with anisotropy clamped to the GPU, fences, present and present modes, object labels for debuggers.
- Device::readBuffer for slow test and debug readback, and DeviceCapabilities fields for debug output, video memory and the API version.
- A GPU test (VXE_GUI_TESTS=1) that checks capabilities, buffer and mapped memory round trips, uploads of all formats and mips, samplers, fences and present against the real driver with zero driver errors or warnings.
### Changed
- Upload and readback rules moved into rhi validation and are shared by both backends: writes need TransferDst, reads need TransferSrc, as in Vulkan.
- Window::glGetProcAddress returns a function pointer, so loading GL needs no cast between function and object pointers.
- The GUI test switch moved into the shared test helpers.

## [#034] 2026-10-27, E6.2 Null backend
### Added
- The full Null device: CpuWrite buffers get real persistent CPU memory for mappedRange, updateBuffer and uploadTexture check ranges, mip levels, layers and data sizes like a GPU backend, and textures beyond the reported limits are refused.
- Pipelines are checked against their shaders: every shader must exist and sit in the right stage.
- submit walks every command, counts it and flags invalid ones (destroyed resources, draws outside a render pass, compute or copies inside one, nested or unclosed passes) in a new DeviceStats::invalidCommands.
- Device stats with object counts and buffer and texture memory, present counting, immediately signaled fences, and a leak report naming every live resource when the device is destroyed.
- Tests for stats and memory sizes, mapped memory, upload checks, valid and invalid command lists, shader stage checks, fences and device limits.
### Fixed
- GCC 14 rejected the narrowing into the 24 bit handle index (-Wconversion); handles are now built through Handle::make, which masks the index.

## [#033] 2026-10-27, E6.1 RHI interface and handles
### Added
- vxe_rhi module (layer 7): typed 32 bit handles (24 bit index, 8 bit generation) and a HandlePool that turns use after destroy into a null lookup.
- RHI types shaped like Vulkan and D3D12 (ADR-003): formats, opt in usage and barrier flags, raster, depth (reversed Z GreaterOrEqual by default), blend and topology state, and descriptions for buffers, textures, samplers, shaders, pipelines, render passes and texture uploads.
- CommandList: a non virtual, flat byte buffer of plain command structs (render passes, binds, draws, indirect and indirect count draws, dispatch, copies, barriers, debug groups, timestamps) with an iterator for backends; reset keeps the memory.
- The Device interface with capabilities, stats, resource creation and destruction, uploads, mapped ranges, fences, submit and present.
- Validation of every description, called by backends on creation, plus mip count and level size helpers.
- vxe_rhi_null module (layer 8) with the Null device skeleton (validated handles, stubs) and createNullDevice.
- Tests for handle typing and staleness, generation wrap, command recording and replay of every payload, validation rules, size helpers and the Null device.
### Docs
- ADR-015: backends expose their own create functions; the factory that picks one lives in render.

## [#032] 2026-10-27, E5.5 Bindings, persistence, conflicts, capture
### Added
- vxe/input/keybinds.hpp: readable binding text ("W", "Ctrl+S", "F3+G", "Mouse Left", "Wheel Up", "key:Z", "Scancode 300") in both directions, forgiving about case and spaces, and keybinds.toml with a commented line per action (name, category, defaults), up to three bindings, unknown actions kept.
- Controls.BindByKeyName (advanced setting): keybinds saved by key label in the current layout instead of physical position, resolved back to the matching key when loaded.
- Conflict detection: same physical input in the same context or an always active one; stacked contexts never conflict because the stack already decides. Swap, unbind and reset (action, category, all) helpers.
- Capture for the keybind screen: the next key with its held modifiers, mouse button or wheel direction; Escape cancels, Backspace clears, a modifier pressed and released alone is captured as itself, and the captured input stays silent until released.
- Platform: layout independent scancode names and lookups by name or by layout label.
- The engine loop registers the input settings and loads and saves config:/keybinds.toml next to settings.toml.
- Tests for binding text round trips, hand written text, TOML round trips, a hand edited file with warnings and unknown actions, conflicts and resolutions, every capture case and the setting.
### Changed
- runApplication loads and saves the player's files through two small helpers.

## [#031] 2026-10-27, E5.4 Raw input and actions
### Added
- vxe_input module (layer 6): InputSystem with per frame raw state (keys by scancode with press edges, mouse buttons, position, summed raw motion, wheel, typed text) and actions of three types: Button (pressed, held, released), Toggle and Axis (wheel notches, or 1 while a key is held).
- Bindings for keys with required modifiers, chords such as F3+G, mouse buttons and wheel directions; up to three per action, with defaults kept for resetting.
- Input contexts on a stack: from the top down a context claims the physical inputs of its active actions, a blocking context (the console) hides everything below it, and always active contexts (Global) are never blocked.
- Taps that go down and up between two frames still count once, key repeats are not new presses, and losing window focus releases everything so no key stays stuck.
- The engine loop updates input from the frame's OS events and exposes it in EngineContext.
- Tests for edges, taps, repeats, toggles, axes, context claiming and blocking, inactive contexts, modifiers, chords, mouse state, focus loss and rebinding.

## [#030] 2026-10-27, E5.3 Console commands
### Added
- vxe/config/console.hpp: CommandRegistry with command descriptions, a quote aware tokenizer, case insensitive command names, "did you mean" hints and autocomplete for the word being typed.
- Settings commands: set (strings take the rest of the line), get, reset with a single id, a category wildcard (Graphics.LOD.*) or everything (*), find over ids and display names, list by category and help.
- parseSettingValue and displaySettingValue for typed text (on, off, yes, no, 1, 0 for booleans, option names in any case), shared with the future console and settings screens.
- Autocomplete covers command names, setting ids (any case), categories, enum options and on or off.
- Tests for tokenizing, every command, helpful failures, wildcard resets, search, completions and custom commands.

## [#029] 2026-10-27, E5.2 Settings persistence and presets
### Added
- vxe/config/settings_file.hpp: settings saved as TOML with one table per category and a comment above every setting (name, description, default, range or options, when it takes effect), enums by name, written atomically; loaded with toml++ through the normal validated set.
- Loading is forgiving: wrong types and impossible values become warnings that name the key, integers are accepted for float settings, enum names ignore case, and invalid TOML fails with its line and column.
- Unknown keys are kept verbatim and written back on save, so older and newer versions do not erase each other's settings.
- Presets: definePreset, applyPreset, presetNames and currentPreset (Custom when nothing matches).
- Global and per world scopes save and load through separate files.
- The engine loop loads config:/settings.toml after onInit, writes the commented defaults on first start, and saves after onShutdown.
- Tests: round trip into a fresh registry, a hand edited file with comments, clamped values, wrong types, case insensitive enums and unknown tables, invalid TOML, files on disk, presets and scopes.

## [#028] 2026-10-27, E5.1 Settings registry
### Added
- vxe_config module (layer 5) with SettingsRegistry: bool, int, float, enum and string settings with display name, description, apply mode, scope, visibility, range and step, enum options and length limits; the category comes from the dotted id.
- Setting<T> handles read with one relaxed atomic load from any thread. Writes from any thread are clamped to range, NaN, unknown enum values, wrong types and too long strings are rejected, and the value is visible immediately.
- Change callbacks with RAII subscriptions run only in dispatchChanges() on the main thread; several changes of one setting in a frame become one callback (oldest to newest), and a change back to the start value is dropped.
- Generic access by id (value, set, reset, resetCategory by prefix, isDefault, find, all, categories) for the console, persistence and the generated settings screen.
- The engine loop owns the registry, exposes it in EngineContext and dispatches setting changes every frame.
- Tests for defaults and metadata, clamping, rejection, dispatch timing and merging, subscription lifetime, writes from another thread, generic access, category resets and repeated registration.
### Changed
- clang-tidy no longer asks for braced return values (modernize-return-braced-init-list).

## [#027] 2026-10-27, E4.5 Application main loop
### Added
- vxe_app module (new top layer 13): Application (onInit, onFrame, onQuitRequested, onShutdown), EngineContext (platform, window, jobs, file system, file watcher, event bus and the frame's OS events) and runApplication.
- runApplication starts the log, platform, runtime folder mounts (config and saves created, assets and textures when present), file watcher, event bus, window and job system, runs the frame loop (OS events, file watcher, main thread tasks, event bus, onFrame) with an idle frame cap, and shuts down in a fixed, documented order.
- The game runs inside the engine loop: VoxelRethinkedApp, with --frames N and --headless on the command line.
- Tests: lifecycle order with a job reporting back on the main thread, a simulated window close ending the loop through onQuitRequested, and a failing onInit returning 1 while still shutting down.
### Docs
- ADR-015 gains layer 13 (app), because Application needs modules above core.

## [#026] 2026-10-27, E4.4 Platform window
### Added
- vxe_platform module (layer 4) on SDL3 3.4.18, with SDL private to the module: Platform (single instance, optional video for headless use), event translation into engine events (quit, resize, pixel size, focus, minimize, display scale, keys by scancode with modifiers, mouse buttons, motion, wheel, text), key names in the current layout and the executable folder.
- Window: windowed, borderless and exclusive fullscreen, high DPI with logical and pixel sizes, relative mouse mode, and an OpenGL 4.6 core context (4.5 fallback) with an optional debug flag. The context and loader are handed to the future OpenGL backend; no GL call happens in the platform module.
- Tests: headless start, single instance, synthetic SDL events translated correctly, and (with VXE_GUI_TESTS=1) a real window that opens with OpenGL 4.6 core debug, resizes, switches borderless, exclusive fullscreen and windowed, toggles relative mouse mode and closes.
- CI installs the X11 and Wayland development packages SDL3 needs on Linux.
### Changed
- vcpkg.json takes SDL3 without its default features: X11 and Wayland on Linux only, no ibus (which would build a whole D-Bus stack).

## [#025] 2026-10-27, E4.3 Catch files created in new folders on Linux
### Fixed
- Linux has no recursive watches, so files written into a brand new sub folder before efsw watched it were missed (Linux CI failure; copying a texture pack folder would hit it). Watches now enable efsw's synthetic events, which report the files already in a newly watched folder; the per file debounce merges any duplicates.

## [#024] 2026-10-27, E4.3 File watcher
### Added
- vxe/io/file_watcher.hpp: FileWatcher over efsw (hidden behind a private implementation), recursive or flat watches, unwatch, and poll() that delivers events on the calling thread.
- Changes are collected per file and delivered once the file has been quiet for the debounce time (200 ms by default), so an editor's burst of writes becomes one reload.
- efsw's thread is stopped before the state it writes to is destroyed.
- Tests that change real files: creation, modification in sub folders, removal, a burst of five writes giving one event, and no events after unwatch. Repeated 20 times in release and under ASan without a failure.

## [#023] 2026-10-27, E4.2 Async IO and compression
### Added
- vxe/io/binary.hpp: BinaryWriter and BinaryReader with little endian on every platform, IEEE float bits, LEB128 and ZigZag varints and length prefixed strings. The reader fails safely with a sticky error flag instead of reading out of bounds.
- vxe/io/compression.hpp: zstd (saves, default level 3) and LZ4 (caches) with allocating and caller buffer variants, size checked decompression and per thread zstd contexts.
- vxe/io/file_io.hpp: readFile and writeFile (atomic temp then rename by default), and AsyncFileIo running reads and writes on the IO threads with completion on the main thread, a worker or the IO thread.
- The io module now depends on jobs; zstd and LZ4 are linked privately.
- Tests for byte order, every value type, varint extremes, malformed input, compression round trips on compressible, random and empty data, corrupt data, buffer reuse, atomic replacement and async completion targets.
### Changed
- clang-tidy skips clang-analyzer-optin.core.EnumCastOutOfRange, which fires inside the MSVC standard library.

## [#022] 2026-10-27, E3.3 Fix job statistics read right after a wait
### Fixed
- The waiting for dependencies count dropped only after the released job was queued, and the completed count rose only after a job counted as done, so a thread that had just waited could read stale numbers (Linux GCC CI failure). Both are now updated before the job can be observed as queued or done. Repeated 200 times in release and debug and 40 times under ASan without a failure.

## [#021] 2026-10-27, E4.1 Virtual file system
### Added
- vxe_io module (layer 3) with Vfs: named mounts (assets, textures, config, saves) over real folders, read only or read write, optionally created on mount, behind a shared mutex for concurrent lookups.
- normalizeRelativePath: separators unified, dot parts removed, and anything that could leave the mount or alias another file rejected (climbing above the root, absolute and drive paths, colons and alternate streams, control and reserved characters, Windows device names, trailing dots and spaces). The same rules apply on every platform.
- resolve, resolveForWrite (refuses read only mounts), exists and list (sorted, non throwing iteration).
- Tests for normalization, every rejection rule, mount resolution, access control, listing and remounting.

## [#020] 2026-10-27, E3.3 Job stats
### Added
- JobSystem::stats(): jobs run and busy time per compute worker, per IO thread and for helping threads, queue depth per priority for both pools, jobs waiting for dependencies, completed and cancelled totals, and jobs per second since the previous call.
- Counters are per thread, cache line aligned and relaxed, so recording them takes no locks.
- Tests for per thread attribution, busy time, per priority depth while a worker is blocked, cancelled and waiting counts, and the rate between calls.
### Performance
- 1,000,000 job stress in release: 0.42 to 0.43 s with the two clock reads per job (0.46 s before), so the timing has no measurable cost.

## [#019] 2026-10-27, E3.2 Dependencies and cancellation
### Added
- JobOptions (priority, cancel token, pool), constructible from a bare Priority so existing calls stay short.
- scheduleAfter: a job queued when all of its JobHandle dependencies finish, without holding a thread while it waits. A finished dependent always implies finished dependencies.
- CancelSource and CancelToken (vxe/jobs/cancel.hpp): jobs cancelled before they start are skipped but still complete; running jobs can check the token and return early.
- runOnMainThread and runMainThreadTasks: tasks from any thread run on the owner thread in posting order, double buffered so tasks may post more tasks.
- IO threads (Pool::Io, 2 by default): IO jobs never run on compute workers or in wait(), so blocking disk work cannot stall CPU work. Without IO threads they fall back to the compute pool.
- Shutdown drains continuations and IO jobs too.
- Tests for chains, diamonds, finished and empty dependencies, 200 way fan out and fan in, cancellation before and during a job, main thread order and nesting, IO isolation and shutdown. Repeated 200 times in release and debug and 40 times under ASan without a failure.
### Fixed
- A finishing job released its dependents before counting itself as done, so a dependent could finish while its dependency still reported unfinished (found by the repeated release runs).

## [#018] 2026-10-27, E3.1 Job scheduler
### Added
- vxe_jobs module (layer 2): JobSystem, a work stealing thread pool with five priorities (Critical to Background). Each worker owns a ring buffer queue per priority (LIFO for its own jobs, FIFO for thieves), other threads submit to shared queues, and the highest priority job anywhere runs first.
- JobCounter for allocation free batches and JobHandle (with a keep alive so dropped handles are safe). wait() runs other jobs while waiting, so nested waits cannot deadlock.
- Idle workers spin briefly, then sleep with a lost wakeup free handshake. The destructor drains every queued and running job before joining.
- vxe/core/inline_function.hpp: move only void() callable with fixed inline storage (48 bytes for jobs), so scheduling never allocates.
- Tests: exactly once execution, 1,000,000 job stress (0.46 s release, 0.97 s ASan), nested waits on 2 workers, strict priority order, handle lifetime, shutdown drain and move only captures. Repeated 50 times in debug and release without a failure.
### Performance
- 1,000,000 tiny jobs scheduled from one thread and run: 0.46 s in release (about 0.46 microseconds per job, dominated by the shared queue mutex). Recorded as the baseline for a lock free queue later.

## [#017] 2026-10-27, E2.3 Projection helpers
### Added
- vxe/math/projection.hpp: perspectiveReversedInfinite (reversed Z, depth 0 to 1, infinite far plane), cameraRelativeView, CameraPosition (chunk plus fraction split exactly from double positions) and relativeToCamera for chunks and blocks with integer differences first.
- Tests: near maps to 1, depth falls towards 0 without reaching it, surfaces 1 block apart at 32,000 blocks get different depths, and camera relative positions stay exact 10 million blocks out.
- doctest printers for ChunkPos, BlockPos and LocalPos in the test helpers.

## [#016] 2026-10-27, E2.2 Fix sign conversion in the raycast
### Fixed
- raycastBlocks indexed std::array with an int axis, which fails -Wsign-conversion on Linux CI (MSVC does not warn). Axes are now usize, with explicit casts for glm's int indices.

## [#015] 2026-10-27, E2.2 Geometry
### Added
- vxe/math/face.hpp: Face in the engine wide order (+X, -X, +Y, -Y, +Z, -Z) with opposite, axis, sign and normal helpers.
- vxe/math/aabb.hpp: Aabb with empty, expand, contains, intersects, translated and inflated.
- vxe/math/frustum.hpp: frustum planes extracted from a view projection matrix, including the degenerate far plane of the reversed Z infinite projection, and a positive vertex box test.
- vxe/math/raycast.hpp: Amanatides and Woo voxel traversal in double precision with a templated solidity predicate, returning block, entry face, distance and point.
- Tests for frustums from standard and reversed infinite projections, box culling, and raycasts along axes, diagonals, negative coordinates, 30 million blocks out, from inside a block and with a zero direction.
### Changed
- Test code may access optionals after doctest REQUIRE (engine/tests/.clang-tidy).

## [#014] 2026-10-27, E2.1 Coordinates and vectors
### Added
- vxe_math module (layer 1) with glm 1.0.3, built with GLM_FORCE_DEPTH_ZERO_TO_ONE and GLM_FORCE_INTRINSICS as public definitions so every file sees one glm layout.
- vxe/math/vector.hpp: Vec, IVec, DVec, Mat4 and Quat aliases.
- vxe/math/coords.hpp: ChunkPos (int32), LocalPos (0 to 15, Y major index), BlockPos (int64), toChunk, toLocal, toBlock, chunkOrigin, blockAt for double positions with clamping, chebyshevDistance without overflow, std::hash and std::formatter specializations.
- Tests for negative coordinates, int32 grid extremes, all 4096 local indices, NaN and huge entity positions, hash spread over 4096 neighbors and log formatting.

## [#013] 2026-10-27, E1.6 Profiling macros
### Added
- vxe/core/profile.hpp: VXE_PROFILE_SCOPE, VXE_PROFILE_FUNCTION, VXE_PROFILE_FRAME, VXE_PROFILE_PLOT and VXE_PROFILE_MESSAGE on Tracy, compiled out unless VXE_ENABLE_PROFILING is on. profiling::setThreadName names threads for Tracy and for debuggers.
- A "profiling" vcpkg feature with the on demand Tracy 0.14.1 client, enabled only by the relwithdebinfo preset.
- Log messages appear in the Tracy timeline. EventBus::dispatch and the game startup are profiled.
- CI jobs for relwithdebinfo on Windows and Linux. The vcpkg cache key now includes the preset.
### Docs
- README explains how to connect Tracy.

## [#012] 2026-10-27, E1.5 Event bus and time
### Added
- vxe/core/event_bus.hpp: typed deferred event bus. publish() from any thread, dispatch() on the owner thread in publish order, RAII Subscription. Handlers may subscribe or unsubscribe while dispatching, and events published during a dispatch wait for the next one.
- vxe/core/time.hpp: steady clock aliases and conversions, Stopwatch, ScopedTimer, and FrameTimer producing FrameTime with clamped and raw deltas.
- Tests for ordering, type routing, subscription lifetime and moves, changes during dispatch, publishing from 4 threads, and frame time clamping with injected time.

## [#011] 2026-10-27, E1.4 Containers, hashing, string ids
### Added
- vxe/core/containers.hpp: HashMap and HashSet on ankerl::unordered_dense, plus StringHash and StringMap for string_view lookups without temporaries.
- vxe/core/hash.hpp: XXH3 hashBytes, hashString and hashValue (padding free types only), constexpr hashCombine and FNV-1a. xxHash is linked privately.
- vxe/core/string_id.hpp: StringId, a 64 bit id hashed at compile time with the _sid literal or at runtime. Debug builds keep every name in a registry and stop on hash collisions.
- vxe/core/small_vector.hpp: SmallVector with inline storage for N elements, tracked heap spill, and std::vector compatible names.
- Tests with reference hash values (XXH3 and FNV-1a), StringId lifetimes, map lookups and SmallVector growth, copies, moves and self push_back.

## [#010] 2026-10-27, E1.3 Memory
### Added
- vxe/core/memory.hpp: memory tags with lock free per tag counters (current bytes, peak, allocation and free counts, one cache line per tag), tracked aligned allocate and deallocate, recordAllocation for external memory, and TrackingAllocator for standard containers.
- LinearAllocator: arena with markers, rewind, reset, peak tracking and typed arrays for trivial types.
- PoolAllocator: O(1) create and destroy on growing pages with an intrusive free list. Pointers stay stable while it grows.
- ScratchScope: a 4 MiB per thread scratch arena created on first use, released when the scope ends, nestable.
- Tests for tag totals, container tracking, alignment, arena overflow, pool slot reuse and per thread scratch arenas.
### Changed
- clang-tidy no longer flags computed array indices, unchecked operator[] or missing math parentheses, and ignores macros for boolean simplification.

## [#009] 2026-10-27, E1.2 Result and Error
### Added
- vxe/core/error.hpp: ErrorCode and Error with code, message and the source location where it was created, plus withContext() and describe().
- vxe/core/result.hpp: Result<T, E> and Result<void, E> on std::variant, with asserted value() and error() access, operator* and ->, valueOr(), and the VXE_TRY macro to pass errors up.
- Shared test helpers (throwing assert handler) and Result tests, including move only values and wrong side access in debug.
### Changed
- toString(Level) is now nameOf(Level), because doctest picks up toString through ADL to print values.
- Engine tests build with DOCTEST_CONFIG_USE_STD_HEADERS so failures can print std::string_view.

## [#008] 2026-10-27, E1.1 Types, asserts, logging
### Added
- vxe/core/types.hpp: fixed width aliases (u8 to u64, i8 to i64, f32, f64, usize, isize).
- vxe/core/assert.hpp: VXE_ASSERT (debug only, condition not evaluated when off), VXE_VERIFY (always), VXE_UNREACHABLE, and a replaceable assert handler. The default handler logs the expression, message and location, breaks into an attached debugger and aborts.
- vxe/core/log.hpp: log categories with atomic levels, a category registry, compile time checked std::format messages, a reusable per thread format buffer, and console plus logs/latest.log outputs. spdlog (built on std::format) stays private to log.cpp.
- The game logs its version to logs/latest.log next to the executable and turns escaping exceptions into a logged error.
- Unit tests for asserts in debug and release, category filtering, the registry and log file creation.
- cmake/triplets/x64-windows-static-md-asan.cmake: vcpkg packages for the Windows asan preset are built with ASan, because MSVC refuses to link ASan code with libraries built without it.
### Changed
- The clang-tidy CMake option is replaced by a tidy target that reads compile_commands.json, because CMake's MSVC wrapper made clang-tidy report exceptions as disabled.
- clang-tidy ignores macros for cognitive complexity and allows do { } while (false) macros.

## [#007] 2026-10-27, E0.4 Fix include check on CMake 3.x
### Fixed
- The include check and rule tests run as cmake -P scripts, which start with old policy defaults on CMake 3.x, so if(IN_LIST) failed on the Linux runners. Both scripts now set cmake_policy(VERSION 3.28). Verified locally with CMake 3.31.6.

## [#006] 2026-10-27, E0.4 CI
### Added
- GitHub Actions workflow: clang-format 22.1.3 check, plus build and test on Windows MSVC (debug, release), Linux Clang 18 (debug, release, asan, tsan) and Linux GCC 14 (debug), with warnings as errors.
- vcpkg is cloned at the pinned baseline and its binary cache is kept between runs with actions/cache.
### Changed
- An empty CXX or VCPKG_DEFAULT_TRIPLET environment variable now counts as unset when picking the Windows defaults.
- UBSan in the asan preset no longer recovers, so undefined behavior fails the run.

## [#005] 2026-10-27, E0.3 Module dependency enforcement
### Added
- cmake/VxeModule.cmake with vxe_add_module(): one layer table for all engine modules, strictly lower layer dependencies only, an allow list for net, glad and OpenGL linkable only PRIVATE by rhi_gl, no engine to game links.
- cmake/CheckIncludes.cmake: rejects includes of engine modules outside the dependency closure and graphics API headers outside rhi_gl, at configure time and again at build time when files change. The game is checked too.
- vxe::vxe convenience target linking every engine module.
- 13 CMake script tests (cmake/tests) proving every rule fires with the right message.
### Changed
- vxe_core is created through vxe_add_module(). The game and vxe_tests link vxe::vxe.
### Docs
- ADR-015 records the layer order.

## [#004] 2026-10-26, E0.2 Tooling
### Added
- .clang-format (LLVM based, 4 spaces, 110 columns, grouped includes) and .clang-tidy (naming rules from the coding standards).
- cmake/Format.cmake with format and format-check targets and a VXE_ENABLE_CLANG_TIDY option. Finds the LLVM tools bundled with Visual Studio.
- .editorconfig and .gitattributes (LF in the repository, binary assets never converted).
### Changed
- .gitignore now also covers IDE folders, CMakeUserPresets.json and the runtime folders logs, captures, saves and screenshots.
- The game prints its version with iostream instead of printf (clang-tidy vararg finding).
### Removed
- .idea project files from the repository (they stay on disk for CLion).
### Docs
- docs/adr with ADR-001 to ADR-014 and an index.
- README mentions the format targets and the ADR folder.

## [#003] 2026-10-26, E0.1 Repository and build skeleton
### Added
- CMakePresets.json with debug, release, relwithdebinfo, asan and tsan presets (Ninja, vcpkg toolchain).
- vcpkg.json manifest with a pinned baseline. Only doctest for now, other libraries arrive with the step that uses them.
- cmake/Warnings.cmake with high warning levels and an opt in warnings as errors switch.
- cmake/Sanitizers.cmake for AddressSanitizer and ThreadSanitizer, including the MSVC ASan runtime copy.
- Targets vxe_core (engine library), voxel_rethinked (game, prints its version) and vxe_tests (doctest, no tests yet).
- README build instructions.
### Changed
- Root CMakeLists.txt now targets CMake 3.28+, C++20 without extensions, MSVC on Windows and one shared bin folder.
- main.cpp moved to game/src/app/main.cpp.
- .gitignore extended with build output and vcpkg folders.

## [#002] 2026-10-26, README
### Docs
- README.md updated to its final state.

## [#001] 2026-10-26, Initial commit
### Added
- CLion starter project, MIT license and README.
