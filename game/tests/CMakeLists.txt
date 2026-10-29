# Game unit tests (Section 24), one executable like the engine's.

find_package(doctest CONFIG REQUIRED)

add_executable(vr_tests
    test_main.cpp
    game_settings_tests.cpp
    game_state_tests.cpp
)
target_link_libraries(vr_tests PRIVATE vr_game doctest::doctest)
target_compile_definitions(vr_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
vxe_set_warnings(vr_tests)
add_test(NAME vr_tests COMMAND vr_tests)
