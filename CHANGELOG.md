# Changelog

All notable changes to Voxel Rethinked. Each entry matches exactly one commit.

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
