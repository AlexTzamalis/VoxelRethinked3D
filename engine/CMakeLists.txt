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

# One target that links every engine module, for the game and tools.
vxe_add_engine_target()

if(VXE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
