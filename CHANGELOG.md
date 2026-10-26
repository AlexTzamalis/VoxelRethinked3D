# Changelog

All notable changes to Voxel Rethinked. Each entry matches exactly one commit.

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
