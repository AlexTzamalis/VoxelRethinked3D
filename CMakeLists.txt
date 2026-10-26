# Root build for Voxel Rethinked and the vxe engine.
# The engine (engine/) is a static library, the game (game/) is an executable
# that links it. The dependency arrow only points from game to engine (ADR-001).
cmake_minimum_required(VERSION 3.28)

# Defaults that must be decided before project() runs, because project() is
# where CMake picks the compiler and where the vcpkg toolchain reads the triplet.
# Both are only defaults: a preset, the command line or the environment wins.
if(CMAKE_HOST_WIN32)
    # MSVC is the primary Windows compiler. Without this, the Ninja generator may
    # pick up a MinGW g++ that happens to be first on PATH.
    if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
        set(CMAKE_CXX_COMPILER cl)
    endif()
    # Static libraries with the dynamic CRT: one executable with no third party
    # DLLs to copy around, while still sharing the CRT with the system.
    if(NOT DEFINED VCPKG_TARGET_TRIPLET AND NOT DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
        set(VCPKG_TARGET_TRIPLET x64-windows-static-md CACHE STRING "vcpkg triplet")
    endif()
endif()

project(VoxelRethinked3D
    VERSION 0.1.0
    DESCRIPTION "Voxel Rethinked, a modern voxel sandbox game, and the vxe engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# We do not use C++20 modules (ADR-002), so skip the per file module dependency
# scan that CMake would otherwise run for every source under C++20.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Every executable (game, tests, tools) lands in one bin folder per build, so
# runtime folders such as config/ and textures/ only need to exist once.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Match the static-md vcpkg triplet: always link the dynamic CRT on MSVC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

option(VXE_BUILD_TESTS "Build the unit test executables" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Warnings)
include(Sanitizers)

if(VXE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)
add_subdirectory(game)
