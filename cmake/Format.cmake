# Code formatting and static analysis helpers.
#
# Targets (each one is only created when its tool is found):
#   format        Rewrites every C++ source file in place with .clang-format.
#   format-check  Fails if any file is not formatted. CI runs this.
#   tidy          Runs clang-tidy (.clang-tidy) on every engine and game source
#                 file, using this build's compile_commands.json.
#
# clang-tidy is a separate target instead of CMAKE_CXX_CLANG_TIDY because
# CMake's MSVC wrapper for it passes the compile flags in a way clang-tidy
# misreads (it reports exceptions as disabled despite /EHsc). Reading the
# compile database gives the exact same flags the compiler sees.

# Visual Studio ships clang-format and clang-tidy in its LLVM folder, which is
# not on PATH even inside a developer shell, so look there too.
set(vxeLlvmHints "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin")

find_program(VXE_CLANG_FORMAT_EXE NAMES clang-format HINTS ${vxeLlvmHints})
find_program(VXE_CLANG_TIDY_EXE NAMES clang-tidy HINTS ${vxeLlvmHints})

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

if(VXE_CLANG_TIDY_EXE)
    file(GLOB_RECURSE vxeTidySources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/engine/*.cpp"
        "${CMAKE_SOURCE_DIR}/game/*.cpp"
        "${CMAKE_SOURCE_DIR}/tools/*.cpp"
        "${CMAKE_SOURCE_DIR}/benchmarks/*.cpp"
    )
    add_custom_target(tidy
        COMMAND ${VXE_CLANG_TIDY_EXE} -p "${CMAKE_BINARY_DIR}" --quiet ${vxeTidySources}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy"
        VERBATIM
    )
else()
    message(STATUS "vxe: clang-tidy not found, the tidy target is unavailable")
endif()
