# Changelog

All notable changes to Voxel Rethinked. Each entry matches exactly one commit.

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
