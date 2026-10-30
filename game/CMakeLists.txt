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
    src/client/block_icons.cpp
    src/client/block_icons.hpp
    src/client/default_pack.hpp
    src/client/demo_world.cpp
    src/client/demo_world.hpp
    src/client/player_controller.cpp
    src/client/player_controller.hpp
    src/common/block_catalog.cpp
    src/common/block_catalog.hpp
    src/common/player_physics.hpp
    src/server/world_saver.cpp
    src/server/world_saver.hpp
    src/server/world_store.cpp
    src/server/world_store.hpp
    src/ui/screens/background_task.hpp
    src/ui/screens/hud_screen.cpp
    src/ui/screens/hud_screen.hpp
    src/ui/screens/inventory_screen.cpp
    src/ui/screens/inventory_screen.hpp
    src/ui/screens/keybinds_screen.cpp
    src/ui/screens/keybinds_screen.hpp
    src/ui/screens/loading_screen.hpp
    src/ui/screens/main_menu_screen.cpp
    src/ui/screens/main_menu_screen.hpp
    src/ui/screens/menu_services.hpp
    src/ui/screens/pause_screen.cpp
    src/ui/screens/pause_screen.hpp
    src/ui/screens/settings_screen.cpp
    src/ui/screens/settings_screen.hpp
    src/ui/screens/widget_sample.cpp
    src/ui/screens/widget_sample.hpp
    src/ui/screens/world_screens.cpp
    src/ui/screens/world_screens.hpp
    src/worldgen/climate.cpp
    src/worldgen/climate.hpp
    src/worldgen/planet.cpp
    src/worldgen/planet.hpp
    src/worldgen/terrain.cpp
    src/worldgen/terrain.hpp
    src/worldgen/terrain_lod_sampler.cpp
    src/worldgen/terrain_lod_sampler.hpp
)

find_package(tomlplusplus CONFIG REQUIRED)

# The default texture pack (textures/default) built into the program as the
# last fallback (Section 15.1): every file becomes a byte array, regenerated
# whenever a file of the pack changes.
set(vrDefaultPackFolder "${PROJECT_SOURCE_DIR}/textures/default")
file(GLOB_RECURSE vrDefaultPackFiles CONFIGURE_DEPENDS "${vrDefaultPackFolder}/*")
# The panorama is hundreds of KB and only decorates the menu: it stays a file
# in textures/default (without it the menu shows the sky).
list(FILTER vrDefaultPackFiles EXCLUDE REGEX "/ui/panorama\.")
set(vrDefaultPackSource "${CMAKE_CURRENT_BINARY_DIR}/generated/default_pack_files.cpp")
add_custom_command(OUTPUT "${vrDefaultPackSource}"
    COMMAND "${CMAKE_COMMAND}" "-DFOLDER=${vrDefaultPackFolder}" "-DOUTPUT=${vrDefaultPackSource}"
            "-DHEADER=client/default_pack.hpp" "-DNAMESPACE=vr" "-DFUNCTION=defaultPackFiles"
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
    DEPENDS ${vrDefaultPackFiles} "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
    COMMENT "Embedding the default texture pack"
    VERBATIM)

add_library(vr_game STATIC ${vrGameSources} "${vrDefaultPackSource}")
target_include_directories(vr_game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vr_game PUBLIC vxe::vxe PRIVATE tomlplusplus::tomlplusplus)
vxe_set_warnings(vr_game)

# The seed map: a world as a picture, without the game (G3.0).
find_package(Stb REQUIRED)
add_executable(vr_seed_map tools/seed_map.cpp)
target_link_libraries(vr_seed_map PRIVATE vr_game)
target_include_directories(vr_seed_map SYSTEM PRIVATE ${Stb_INCLUDE_DIR})

add_executable(voxel_rethinked src/app/main.cpp)
target_link_libraries(voxel_rethinked PRIVATE vr_game)
vxe_set_warnings(voxel_rethinked)

# The engine reads its shaders from shaders/ next to the executable
# (EngineConfig::shaderFolder) and the game its data from assets/ (the
# assets:/ mount), so every build copies both there.
# A target of its own, not a step after linking: a changed texture or shader
# must arrive even when the program did not relink.
add_custom_target(vr_runtime_files ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/engine/shaders"
            "$<TARGET_FILE_DIR:voxel_rethinked>/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:voxel_rethinked>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/textures"
            "$<TARGET_FILE_DIR:voxel_rethinked>/textures"
    COMMENT "Copying engine shaders, game assets and texture packs next to voxel_rethinked"
    VERBATIM)
add_dependencies(voxel_rethinked vr_runtime_files)

# The game may use any engine module, but never the graphics API directly.
set(vrCheckedSources ${vrGameSources} src/app/main.cpp)
list(TRANSFORM vrCheckedSources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
vxe_add_include_check(voxel_rethinked ANY_MODULE
    OWNER "game"
    FILES ${vrCheckedSources})

if(VXE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
