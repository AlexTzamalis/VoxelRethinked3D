# Engine unit tests. One executable holds every engine test so the test run
# stays fast and the doctest main is compiled once.

find_package(doctest CONFIG REQUIRED)

add_executable(vxe_tests
    test_helpers.hpp
    test_main.cpp
    app/application_tests.cpp
    config/console_tests.cpp
    config/settings_file_tests.cpp
    config/settings_tests.cpp
    core/assert_tests.cpp
    core/containers_tests.cpp
    core/event_time_tests.cpp
    core/log_tests.cpp
    core/memory_tests.cpp
    core/profile_tests.cpp
    core/result_tests.cpp
    core/stats_tests.cpp
    debug/debug_ui_tests.cpp
    debug/developer_console_tests.cpp
    debug/overlay_tests.cpp
    input/input_tests.cpp
    input/keybinds_tests.cpp
    io/binary_compression_tests.cpp
    io/file_io_tests.cpp
    io/file_watcher_tests.cpp
    io/vfs_tests.cpp
    jobs/job_dependency_tests.cpp
    jobs/job_stats_tests.cpp
    jobs/job_system_tests.cpp
    math/coords_tests.cpp
    math/geometry_tests.cpp
    math/projection_tests.cpp
    platform/platform_tests.cpp
    render/camera_tests.cpp
    render/debug_draw_tests.cpp
    render/frame_graph_tests.cpp
    render/sky_tests.cpp
    rhi/gl_device_tests.cpp
    rhi/null_device_tests.cpp
    rhi/rhi_tests.cpp
    rhi/shader_source_tests.cpp
)
# SDL is linked directly only so platform tests can push synthetic OS events.
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(vxe_tests PRIVATE vxe::vxe doctest::doctest SDL3::SDL3)
# doctest forward declares std types by default; with real std headers it can
# print std::string_view and other values in failure messages.
target_compile_definitions(vxe_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
    # The GPU tests compile the real engine shaders from the source tree.
    "VXE_ENGINE_SHADER_DIR=\"${PROJECT_SOURCE_DIR}/engine/shaders\"")
vxe_set_warnings(vxe_tests)

# Registered as a single CTest test. doctest returns a non zero exit code when
# any test case fails, which is all CTest needs.
add_test(NAME vxe_tests COMMAND vxe_tests)
