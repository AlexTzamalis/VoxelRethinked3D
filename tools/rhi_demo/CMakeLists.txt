# The RHI demo (E6.5): a spinning cube and a triangle through the OpenGL
# backend, with GPU and CPU frame times in the window title.

add_executable(vxe_rhi_demo main.cpp)
target_link_libraries(vxe_rhi_demo PRIVATE vxe::vxe)
# A developer tool, so it reads the engine shaders from the source tree.
target_compile_definitions(vxe_rhi_demo PRIVATE
    "VXE_ENGINE_SHADER_DIR=\"${PROJECT_SOURCE_DIR}/engine/shaders\"")
vxe_set_warnings(vxe_rhi_demo)
