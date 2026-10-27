# Build time shader validation (E6.4).
#
# vxe_add_shader_check(<target> ROOT <shader root folder> [DEFINES NAME[=VALUE]...])
#
# Creates <target>, built as part of ALL, that runs vxe_shader_check on every
# .vert, .frag and .comp file under ROOT. A shader that does not compile fails
# the build with "<file>:<line>: error: ..." messages. Each shader has its own
# stamp file, so only shaders whose text (or any include file under ROOT)
# changed are checked again.
#
# The include files (.glsl) are not checked on their own, because they are
# not complete shaders; they are checked through every shader that uses them.

function(vxe_add_shader_check target)
    cmake_parse_arguments(arg "" "ROOT" "DEFINES" ${ARGN})
    if(NOT arg_ROOT)
        message(FATAL_ERROR "vxe_add_shader_check(${target}) needs ROOT")
    endif()

    file(GLOB_RECURSE stages CONFIGURE_DEPENDS
        "${arg_ROOT}/*.vert" "${arg_ROOT}/*.frag" "${arg_ROOT}/*.comp")
    file(GLOB_RECURSE includes CONFIGURE_DEPENDS "${arg_ROOT}/*.glsl")

    set(defineArgs "")
    foreach(define IN LISTS arg_DEFINES)
        list(APPEND defineArgs -D "${define}")
    endforeach()

    set(stamps "")
    foreach(stage IN LISTS stages)
        file(RELATIVE_PATH relative "${arg_ROOT}" "${stage}")
        set(stamp "${CMAKE_CURRENT_BINARY_DIR}/shader_checks/${target}/${relative}.ok")
        get_filename_component(stampDir "${stamp}" DIRECTORY)
        file(MAKE_DIRECTORY "${stampDir}")
        # The second command only runs if the check passed, so a failed shader
        # leaves no stamp and is checked again on the next build.
        add_custom_command(
            OUTPUT "${stamp}"
            COMMAND vxe_shader_check --root "${arg_ROOT}" ${defineArgs} "${relative}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
            DEPENDS "${stage}" ${includes} vxe_shader_check
            COMMENT "Checking shader ${relative}"
            VERBATIM
        )
        list(APPEND stamps "${stamp}")
    endforeach()

    add_custom_target(${target} ALL DEPENDS ${stamps} SOURCES ${stages} ${includes})
endfunction()
