# The Voxel Rethinked game. It links the engine and never the other way round.

add_executable(voxel_rethinked
    src/app/main.cpp
)
target_link_libraries(voxel_rethinked PRIVATE vxe::core)
vxe_set_warnings(voxel_rethinked)
