# Include rule checks for engine modules and the game.
#
# Linking rules (VxeModule.cmake) stop a module from linking a higher layer, but
# every engine header lives under one include root (engine/include/vxe/...), so
# a source file could still include a header of a module it does not depend on.
# This check scans #include lines and rejects:
#   1. <vxe/X/...> or "vxe/X/..." where module X is not in the allowed list.
#   2. Graphics API headers (glad, GL, KHR) unless explicitly allowed. Only the
#      OpenGL backend (engine/src/rhi_gl) may see them (ADR-003).
#
# The file works in two ways:
#   - include()d at configure time, which defines vxe_check_includes().
#   - run as a script (cmake -P) by a build step, so files edited after
#     configuring are checked again. See vxe_add_include_check() in VxeModule.cmake.

# Script mode (cmake -P) starts with old policy defaults on CMake 3.x, where
# if(IN_LIST) does not exist yet. include() gets its own policy scope, so this
# does not leak into the including project.
cmake_policy(VERSION 3.28)

# Checks the include lines of a list of files.
#   OWNER              Name used in error messages, for example "module core".
#   ALLOWED_MODULES    Engine modules whose headers may be included.
#   ANY_MODULE         Flag: any engine module may be included (the game).
#   ALLOW_GRAPHICS_API Flag: glad and GL headers are allowed (rhi_gl only).
#   FILES              Absolute paths of the files to scan.
function(vxe_check_includes)
    cmake_parse_arguments(arg "ANY_MODULE;ALLOW_GRAPHICS_API" "OWNER" "ALLOWED_MODULES;FILES" ${ARGN})

    set(violations "")
    foreach(path IN LISTS arg_FILES)
        if(NOT EXISTS "${path}")
            continue()
        endif()
        file(STRINGS "${path}" includeLines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
        foreach(line IN LISTS includeLines)
            if(NOT arg_ALLOW_GRAPHICS_API AND line MATCHES "[<\"](glad/|GL/|GLES[0-9]*/|KHR/)")
                list(APPEND violations "${path}: graphics API header outside rhi_gl: ${line}")
            endif()
            if(NOT arg_ANY_MODULE AND line MATCHES "[<\"]vxe/([A-Za-z0-9_]+)/")
                set(includedModule "${CMAKE_MATCH_1}")
                if(NOT includedModule IN_LIST arg_ALLOWED_MODULES)
                    list(APPEND violations
                        "${path}: ${arg_OWNER} includes a header of module '${includedModule}' it does not depend on: ${line}")
                endif()
            endif()
        endforeach()
    endforeach()

    if(violations)
        list(JOIN violations "\n  " message)
        message(FATAL_ERROR "vxe include rule violation:\n  ${message}\n"
            "Add the module to DEPENDS if the layer order allows it (docs/adr/0015-module-layer-order.md).")
    endif()
endfunction()

# Script mode entry point, used by the build time check. VXE_CHECK_ARGS_FILE is
# only defined on that command line, so a plain include() never runs this.
if(DEFINED VXE_CHECK_ARGS_FILE)
    include("${VXE_CHECK_ARGS_FILE}")
    set(flags "")
    if(VXE_CHECK_ANY_MODULE)
        list(APPEND flags ANY_MODULE)
    endif()
    if(VXE_CHECK_ALLOW_GRAPHICS_API)
        list(APPEND flags ALLOW_GRAPHICS_API)
    endif()
    vxe_check_includes(${flags}
        OWNER "${VXE_CHECK_OWNER}"
        ALLOWED_MODULES ${VXE_CHECK_ALLOWED_MODULES}
        FILES ${VXE_CHECK_FILES})
    file(TOUCH "${VXE_CHECK_STAMP}")
endif()
