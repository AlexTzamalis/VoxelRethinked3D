# Game unit tests (Section 24), one executable like the engine's.

find_package(doctest CONFIG REQUIRED)

add_executable(vr_tests
    test_main.cpp
    block_catalog_tests.cpp
    game_settings_tests.cpp
    game_state_tests.cpp
)
target_link_libraries(vr_tests PRIVATE vr_game doctest::doctest)
target_compile_definitions(vr_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
    # The real block definitions, so every shipped file is checked by the tests.
    "VR_BLOCKS_DIR=\"${PROJECT_SOURCE_DIR}/game/assets/data/blocks\"")
vxe_set_warnings(vr_tests)
add_test(NAME vr_tests COMMAND vr_tests)
