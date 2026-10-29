# The RHI demo (E6.5): a spinning cube and a triangle through the OpenGL
# backend, with GPU and CPU frame times in the window title, and with
# --terrain a field of chunks through the chunk renderer (E9.5).

add_executable(vxe_rhi_demo main.cpp terrain_field.cpp)
target_link_libraries(vxe_rhi_demo PRIVATE vxe::vxe)
# A developer tool, so it reads the engine shaders from the source tree.
target_compile_definitions(vxe_rhi_demo PRIVATE
    "VXE_ENGINE_SHADER_DIR=\"${PROJECT_SOURCE_DIR}/engine/shaders\"")
# The synthetic terrain sample for --terrain is shared with the unit tests.
target_include_directories(vxe_rhi_demo PRIVATE "${PROJECT_SOURCE_DIR}/engine/tests")
vxe_set_warnings(vxe_rhi_demo)
