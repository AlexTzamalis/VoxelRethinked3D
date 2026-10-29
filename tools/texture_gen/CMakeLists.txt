# The texture generator (G2.1): paints the default texture pack from the
# recipes in default_pack.toml (see that file for how to run it).

find_package(tomlplusplus CONFIG REQUIRED)
find_package(Stb REQUIRED)

add_library(vxe_texture_painter STATIC painter.cpp)
target_include_directories(vxe_texture_painter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
vxe_set_warnings(vxe_texture_painter)

add_executable(vxe_texture_gen main.cpp)
target_link_libraries(vxe_texture_gen PRIVATE vxe_texture_painter tomlplusplus::tomlplusplus)
# Third party headers: their warnings are not ours.
target_include_directories(vxe_texture_gen SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
vxe_set_warnings(vxe_texture_gen)

if(VXE_BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    add_executable(vxe_texture_gen_tests tests/painter_tests.cpp)
    target_link_libraries(vxe_texture_gen_tests PRIVATE vxe_texture_painter doctest::doctest)
    vxe_set_warnings(vxe_texture_gen_tests)
    add_test(NAME texture_gen_tests COMMAND vxe_texture_gen_tests)
endif()
