# The Voxel Rethinked game. It links the engine and never the other way round.
#
# The game code is a static library (vr_game) so the executable and the game
# tests (vr_tests) share it; the executable only adds main().

set(vrGameSources
    src/app/game_settings.cpp
    src/app/game_settings.hpp
    src/app/game_state.cpp
    src/app/game_state.hpp
    src/app/voxel_rethinked_app.cpp
    src/app/voxel_rethinked_app.hpp
    src/common/block_catalog.cpp
    src/common/block_catalog.hpp
)

find_package(tomlplusplus CONFIG REQUIRED)

add_library(vr_game STATIC ${vrGameSources})
target_include_directories(vr_game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vr_game PUBLIC vxe::vxe PRIVATE tomlplusplus::tomlplusplus)
vxe_set_warnings(vr_game)

add_executable(voxel_rethinked src/app/main.cpp)
target_link_libraries(voxel_rethinked PRIVATE vr_game)
vxe_set_warnings(voxel_rethinked)

# The engine reads its shaders from shaders/ next to the executable
# (EngineConfig::shaderFolder) and the game its data from assets/ (the
# assets:/ mount), so every build copies both there.
add_custom_command(TARGET voxel_rethinked POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/engine/shaders"
            "$<TARGET_FILE_DIR:voxel_rethinked>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:voxel_rethinked>/assets"
    COMMENT "Copying engine shaders and game assets next to voxel_rethinked"
    VERBATIM)

# The game may use any engine module, but never the graphics API directly.
set(vrCheckedSources ${vrGameSources} src/app/main.cpp)
list(TRANSFORM vrCheckedSources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
vxe_add_include_check(voxel_rethinked ANY_MODULE
    OWNER "game"
    FILES ${vrCheckedSources})

if(VXE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
