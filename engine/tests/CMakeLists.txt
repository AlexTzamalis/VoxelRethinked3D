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
    core/offset_allocator_tests.cpp
    core/profile_tests.cpp
    core/result_tests.cpp
    core/stats_tests.cpp
    debug/debug_ui_tests.cpp
    debug/developer_console_tests.cpp
    debug/overlay_tests.cpp
    input/input_tests.cpp
    input/keybinds_tests.cpp
    io/binary_compression_tests.cpp
    audio/audio_tests.cpp
    io/file_io_tests.cpp
    io/file_watcher_tests.cpp
    io/vfs_tests.cpp
    jobs/job_dependency_tests.cpp
    jobs/job_stats_tests.cpp
    jobs/job_system_tests.cpp
    math/coords_tests.cpp
    math/geometry_tests.cpp
    math/noise_tests.cpp
    math/projection_tests.cpp
    platform/platform_tests.cpp
    render/camera_tests.cpp
    render/cloud_tests.cpp
    render/chunk_culling_tests.cpp
    render/chunk_renderer_tests.cpp
    render/lod_renderer_tests.cpp
    render/texture_pack_tests.cpp
    render/debug_draw_tests.cpp
    render/frame_graph_tests.cpp
    render/frame_pacer_tests.cpp
    render/post_process_tests.cpp
    render/sky_tests.cpp
    rhi/gl_device_tests.cpp
    rhi/null_device_tests.cpp
    rhi/rhi_tests.cpp
    rhi/shader_source_tests.cpp
    ui/controls_tests.cpp
    ui/focus_tests.cpp
    ui/keybinds_view_tests.cpp
    ui/pixel_font_tests.cpp
    ui/quad_batch_tests.cpp
    ui/screen_tests.cpp
    ui/widget_tests.cpp
    voxel/light_tests.cpp
    voxel/block_registry_tests.cpp
    voxel/chunk_light_tests.cpp
    voxel/lod_tests.cpp
    voxel/lod_manager_tests.cpp
    voxel/lod_mesher_tests.cpp
    voxel/lod_premapper_tests.cpp
    voxel/lod_downsample_tests.cpp
    voxel/lod_render_sink_tests.cpp
    voxel/chunk_storage_tests.cpp
    voxel/mesher_tests.cpp
    voxel/region_tests.cpp
    voxel/streaming_tests.cpp
    voxel/voxel_world_tests.cpp
)
# A program the region crash test starts and kills while it commits (E9.8).
add_executable(vxe_region_crash_writer tools/region_crash_writer.cpp)
target_link_libraries(vxe_region_crash_writer PRIVATE vxe::vxe)
vxe_set_warnings(vxe_region_crash_writer)
add_dependencies(vxe_tests vxe_region_crash_writer)

# SDL is linked directly only so platform tests can push synthetic OS events
# (and the region crash test can start and kill a process).
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(vxe_tests PRIVATE vxe::vxe doctest::doctest SDL3::SDL3)
# stb_image_write makes the PNGs of the texture pack tests.
find_package(Stb REQUIRED)
target_include_directories(vxe_tests SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# doctest forward declares std types by default; with real std headers it can
# print std::string_view and other values in failure messages.
target_compile_definitions(vxe_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
    # The GPU tests compile the real engine shaders from the source tree.
    "VXE_ENGINE_SHADER_DIR=\"${PROJECT_SOURCE_DIR}/engine/shaders\""
    "VXE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    "VXE_TEST_SOUND=\"${PROJECT_SOURCE_DIR}/sounds/ui/mouse-click-ui-1.mp3\""
    "VXE_REGION_CRASH_WRITER=\"$<TARGET_FILE:vxe_region_crash_writer>\"")
vxe_set_warnings(vxe_tests)

# Registered as a single CTest test. doctest returns a non zero exit code when
# any test case fails, which is all CTest needs.
add_test(NAME vxe_tests COMMAND vxe_tests)
