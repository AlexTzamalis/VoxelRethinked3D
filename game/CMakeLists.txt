# The Voxel Rethinked game. It links the engine and never the other way round.

set(vrGameSources
    src/app/main.cpp
    src/app/voxel_rethinked_app.cpp
    src/app/voxel_rethinked_app.hpp
)

add_executable(voxel_rethinked ${vrGameSources})
target_link_libraries(voxel_rethinked PRIVATE vxe::vxe)
vxe_set_warnings(voxel_rethinked)

# The game may use any engine module, but never the graphics API directly.
list(TRANSFORM vrGameSources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
vxe_add_include_check(voxel_rethinked ANY_MODULE
    OWNER "game"
    FILES ${vrGameSources})
