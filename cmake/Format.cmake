# Code formatting and static analysis helpers.
#
# Targets (only created when clang-format is found):
#   format        Rewrites every C++ source file in place with .clang-format.
#   format-check  Fails if any file is not formatted. CI runs this.
#
# Option:
#   VXE_ENABLE_CLANG_TIDY  Runs clang-tidy on every file we compile, using
#                          .clang-tidy. Off by default because it roughly
#                          doubles build times.

# Visual Studio ships clang-format and clang-tidy in its LLVM folder, which is
# not on PATH even inside a developer shell, so look there too.
set(vxeLlvmHints "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin")

find_program(VXE_CLANG_FORMAT_EXE NAMES clang-format HINTS ${vxeLlvmHints})
find_program(VXE_CLANG_TIDY_EXE NAMES clang-tidy HINTS ${vxeLlvmHints})

option(VXE_ENABLE_CLANG_TIDY "Run clang-tidy while compiling our targets" OFF)

if(VXE_CLANG_FORMAT_EXE)
    # A glob is acceptable here (unlike for target sources): CONFIGURE_DEPENDS
    # picks up new files, and these targets only run by hand or in CI.
    file(GLOB_RECURSE vxeFormatSources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/engine/*.hpp" "${CMAKE_SOURCE_DIR}/engine/*.cpp"
        "${CMAKE_SOURCE_DIR}/game/*.hpp" "${CMAKE_SOURCE_DIR}/game/*.cpp"
        "${CMAKE_SOURCE_DIR}/tools/*.hpp" "${CMAKE_SOURCE_DIR}/tools/*.cpp"
        "${CMAKE_SOURCE_DIR}/benchmarks/*.hpp" "${CMAKE_SOURCE_DIR}/benchmarks/*.cpp"
    )

    add_custom_target(format
        COMMAND ${VXE_CLANG_FORMAT_EXE} -i --style=file ${vxeFormatSources}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Formatting C++ sources with clang-format"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${VXE_CLANG_FORMAT_EXE} --dry-run --Werror --style=file ${vxeFormatSources}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking C++ formatting with clang-format"
        VERBATIM
    )
else()
    message(STATUS "vxe: clang-format not found, the format and format-check targets are unavailable")
endif()

if(VXE_ENABLE_CLANG_TIDY)
    if(NOT VXE_CLANG_TIDY_EXE)
        message(FATAL_ERROR "VXE_ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
    endif()
    # Set before add_subdirectory(), so every target we define picks it up.
    # vcpkg packages are prebuilt and never pass through this.
    set(CMAKE_CXX_CLANG_TIDY "${VXE_CLANG_TIDY_EXE}")
endif()
