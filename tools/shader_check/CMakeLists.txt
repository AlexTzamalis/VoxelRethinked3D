# The build time shader checker (E6.4). cmake/Shaders.cmake runs it on every
# shader stage file, so an invalid shader fails the build with file:line errors.

find_package(glslang CONFIG REQUIRED)

add_executable(vxe_shader_check main.cpp)
target_link_libraries(vxe_shader_check PRIVATE
    vxe::rhi
    glslang::glslang
    glslang::glslang-default-resource-limits
)
vxe_set_warnings(vxe_shader_check)

if(VXE_BUILD_TESTS)
    # Each case runs the checker on a broken shader and expects it to fail
    # with a message that names the right file and line.
    set(shaderCheckTests "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    function(vxe_add_shader_check_failure_test name shader expected)
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                "-DTOOL=$<TARGET_FILE:vxe_shader_check>"
                "-DROOT=${shaderCheckTests}"
                "-DSHADER=${shader}"
                "-DEXPECTED=${expected}"
                -P "${shaderCheckTests}/expect_failure.cmake")
    endfunction()
    vxe_add_shader_check_failure_test(shader_check_reports_errors
        broken.frag "tests/broken.frag:4: error: 'undefinedValue' : undeclared identifier")
    vxe_add_shader_check_failure_test(shader_check_names_include_files
        broken_include.frag "tests/include/broken.glsl:5: error: ")
    vxe_add_shader_check_failure_test(shader_check_reports_missing_includes
        missing_include.frag "tests/missing_include.frag:2: cannot include 'include/missing.glsl'")
endif()
