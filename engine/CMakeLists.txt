# The vxe engine. Each module is its own static library created with
# vxe_add_module() (cmake/VxeModule.cmake), which enforces the layer order.
# Add modules from the lowest layer up, in the order of VXE_MODULE_LAYERS.

set(VXE_ENGINE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# spdlog is built on std::format (vcpkg.json disables its fmt feature) and is
# linked privately: only log.cpp sees it, the public log API does not.
find_package(spdlog CONFIG REQUIRED)
find_package(unordered_dense CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

vxe_add_module(core
    SOURCES
        src/core/assert.cpp
        src/core/error.cpp
        src/core/event_bus.cpp
        src/core/hash.cpp
        src/core/linear_allocator.cpp
        src/core/log.cpp
        src/core/memory.cpp
        src/core/profile.cpp
        src/core/scratch.cpp
        src/core/stats.cpp
        src/core/string_id.cpp
        src/core/time.cpp
        src/core/version.cpp
    PUBLIC_LIBS
        unordered_dense::unordered_dense
    PRIVATE_LIBS
        spdlog::spdlog
        xxHash::xxhash
)
target_compile_definitions(vxe_core PRIVATE
    VXE_VERSION_STRING="${PROJECT_VERSION}"
)

# Tracy profiler client (E1.6). The relwithdebinfo preset turns this on and
# also enables the "profiling" vcpkg feature that installs Tracy. Linked
# PUBLIC because the VXE_PROFILE_* macros expand to Tracy macros in every
# file that uses them.
option(VXE_ENABLE_PROFILING "Build with the Tracy profiler client" OFF)
if(VXE_ENABLE_PROFILING)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(vxe_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(vxe_core PUBLIC VXE_ENABLE_PROFILING=1)
    message(STATUS "vxe: Tracy profiling enabled")
endif()

# Math: glm wrappers and the coordinate types of the voxel world (E2). The glm
# switches are PUBLIC definitions so every file sees the same glm layout:
# depth 0 to 1 matches the reversed Z projection (ADR-010), intrinsics give
# SIMD versions of the vector math.
find_package(glm CONFIG REQUIRED)

vxe_add_module(math
    SOURCES
        src/math/coords.cpp
        src/math/frustum.cpp
        src/math/projection.cpp
    DEPENDS
        core
    PUBLIC_LIBS
        glm::glm
)
target_compile_definitions(vxe_math PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_INTRINSICS
)

# Jobs: the work stealing scheduler with priorities (E3, ADR-012).
find_package(Threads REQUIRED)

vxe_add_module(jobs
    SOURCES
        src/jobs/job_system.cpp
    DEPENDS
        core
    PUBLIC_LIBS
        Threads::Threads
)

# IO: virtual file system, async file IO, compression, serialization (E4).
find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(efsw CONFIG REQUIRED)

vxe_add_module(io
    SOURCES
        src/io/binary.cpp
        src/io/compression.cpp
        src/io/file_io.cpp
        src/io/file_watcher.cpp
        src/io/vfs.cpp
    DEPENDS
        core
        jobs
    PRIVATE_LIBS
        zstd::libzstd
        lz4::lz4
        efsw::efsw
)

# Platform: SDL3 window, OpenGL context creation, OS events, paths (E4.4, ADR-004).
# SDL is private: only the platform sources see it.
find_package(SDL3 CONFIG REQUIRED)

vxe_add_module(platform
    SOURCES
        src/platform/frame_limiter.cpp
        src/platform/platform.cpp
        src/platform/window.cpp
    DEPENDS
        core
        math
    PRIVATE_LIBS
        SDL3::SDL3
)

# Config: the settings registry and, later, persistence and console commands (E5).
find_package(tomlplusplus CONFIG REQUIRED)

vxe_add_module(config
    SOURCES
        src/config/console.cpp
        src/config/settings.cpp
        src/config/settings_file.cpp
    DEPENDS
        core
        io
    PRIVATE_LIBS
        tomlplusplus::tomlplusplus
)

# Input: raw device state, actions, contexts and later keybinds (E5.4, E5.5).
vxe_add_module(input
    SOURCES
        src/input/input.cpp
        src/input/keybinds.cpp
    DEPENDS
        core
        math
        io
        platform
        config
    PRIVATE_LIBS
        tomlplusplus::tomlplusplus
)

# RHI: the backend independent graphics interface (E6, ADR-003), and the Null
# backend for tests and headless servers.
# The factory that picks a backend lives in render, above both backends
# (ADR-015), so rhi itself never depends on a backend.
vxe_add_module(rhi
    SOURCES
        src/rhi/command_list.cpp
        src/rhi/gpu_timer.cpp
        src/rhi/shader_source.cpp
        src/rhi/validation.cpp
    DEPENDS
        core
)

vxe_add_module(rhi_null
    SOURCES
        src/rhi_null/null_device.cpp
    DEPENDS
        core
        rhi
)

# The OpenGL backend (E6.3 to E6.6). The only module that sees GL: the glad
# loader is linked PRIVATE, and the include check allows GL headers only here
# (ADR-003). The loader comes from the generated sources in third_party/glad.
add_subdirectory(third_party/glad)

vxe_add_module(rhi_gl
    SOURCES
        src/rhi_gl/gl_commands.cpp
        src/rhi_gl/gl_commands.hpp
        src/rhi_gl/gl_device.cpp
        src/rhi_gl/gl_formats.cpp
        src/rhi_gl/gl_formats.hpp
        src/rhi_gl/gl_resources.hpp
        src/rhi_gl/gl_state.cpp
        src/rhi_gl/gl_state.hpp
    DEPENDS
        core
        platform
        rhi
    PRIVATE_LIBS
        glad::glad
)

# Render: the renderer on top of the RHI (E8). It records command lists and
# never sees a graphics API (ADR-003).
vxe_add_module(render
    SOURCES
        src/render/camera.cpp
        src/render/cpu_write_ring.cpp
        src/render/debug_draw.cpp
        src/render/frame_constants.cpp
        src/render/frame_graph.cpp
    DEPENDS
        core
        math
        config
        input
        rhi
)

# Engine shaders (GLSL 460 with the engine's include system). Checked with
# glslang on every build, so a broken shader fails the build (E6.4).
vxe_add_shader_check(vxe_engine_shaders ROOT "${CMAKE_CURRENT_SOURCE_DIR}/shaders")

# Debug: developer tools on Dear ImGui and ImPlot (E7, ADR-013), drawn
# through the RHI. ImGui is linked PUBLIC because debug pages and game
# developer panels call ImGui directly. The stat registry itself lives in
# core (ADR-017).
find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)

vxe_add_module(debug
    SOURCES
        src/debug/debug_ui.cpp
        src/debug/developer_console.cpp
        src/debug/imgui_renderer.cpp
        src/debug/overlay.cpp
        src/debug/stat_csv.cpp
    DEPENDS
        core
        math
        io
        platform
        config
        input
        rhi
    PUBLIC_LIBS
        imgui::imgui
        implot::implot
)

# App: Application, EngineContext and the main loop (E4.5). The top layer,
# because the loop drives every other module (ADR-015).
vxe_add_module(app
    SOURCES
        src/app/application.cpp
    DEPENDS
        core
        jobs
        io
        platform
        config
        input
)

# One target that links every engine module, for the game and tools.
vxe_add_engine_target()

if(VXE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
