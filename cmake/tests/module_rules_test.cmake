# Script tests for the engine build rules (cmake/VxeModule.cmake and
# cmake/CheckIncludes.cmake). Run as: cmake -DCASE=<name> -DWORK_DIR=<dir> -P <this file>
# Every rule violation must stop CMake with a clear message, and every legal
# setup must pass. The expected output of each case lives in CMakeLists.txt.

# Script mode (cmake -P) starts with old policy defaults on CMake 3.x, where
# if(IN_LIST) does not exist yet. include() gets its own policy scope, so this
# does not leak into the including project.
cmake_policy(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/../VxeModule.cmake")

# Writes a source file with the given include line and runs the include check on it.
function(checkIncludeLine fileName line)
    set(path "${WORK_DIR}/${fileName}")
    file(WRITE "${path}" "${line}\nint unused = 0;\n")
    vxe_check_includes(${ARGN} FILES "${path}")
endfunction()

if(CASE STREQUAL "valid_dependencies")
    vxe_check_module_dependency(math core)
    vxe_check_module_dependency(render rhi)
    vxe_check_module_dependency(rhi_gl rhi)
    vxe_check_module_dependency(voxel render)
    vxe_check_module_dependency(net io)
elseif(CASE STREQUAL "upward_dependency")
    vxe_check_module_dependency(core render)
elseif(CASE STREQUAL "same_layer_dependency")
    vxe_check_module_dependency(rhi_null rhi_gl)
elseif(CASE STREQUAL "net_allow_list")
    vxe_check_module_dependency(net math)
elseif(CASE STREQUAL "unknown_module")
    vxe_check_module_dependency(core banana)
elseif(CASE STREQUAL "valid_libs")
    vxe_check_module_libs(rhi_gl PRIVATE glad::glad OpenGL::GL)
    vxe_check_module_libs(core PUBLIC doctest::doctest)
elseif(CASE STREQUAL "graphics_lib_outside_rhi_gl")
    vxe_check_module_libs(render PRIVATE glad::glad)
elseif(CASE STREQUAL "graphics_lib_public")
    vxe_check_module_libs(rhi_gl PUBLIC glad::glad)
elseif(CASE STREQUAL "engine_links_game")
    vxe_check_module_libs(core PRIVATE voxel_rethinked)
elseif(CASE STREQUAL "valid_includes")
    checkIncludeLine(own.cpp "#include <vxe/core/version.hpp>" OWNER "module core" ALLOWED_MODULES core)
    checkIncludeLine(quoted.cpp "#include \"vxe/core/version.hpp\"" OWNER "module math" ALLOWED_MODULES math core)
    checkIncludeLine(gl.cpp "#include <glad/gl.h>" ALLOW_GRAPHICS_API OWNER "module rhi_gl" ALLOWED_MODULES rhi_gl)
    checkIncludeLine(game.cpp "#include <vxe/render/renderer.hpp>" ANY_MODULE OWNER "game")
elseif(CASE STREQUAL "include_of_higher_module")
    checkIncludeLine(bad.cpp "#include <vxe/render/renderer.hpp>" OWNER "module core" ALLOWED_MODULES core)
elseif(CASE STREQUAL "include_of_glad")
    checkIncludeLine(bad_glad.cpp "#include <glad/gl.h>" OWNER "module render" ALLOWED_MODULES render)
elseif(CASE STREQUAL "game_includes_gl")
    checkIncludeLine(bad_game.cpp "#  include <GL/gl.h>" ANY_MODULE OWNER "game")
else()
    message(FATAL_ERROR "Unknown test case '${CASE}'")
endif()

message(STATUS "VXE_RULE_TEST_PASSED")
