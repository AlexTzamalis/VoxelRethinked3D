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

# One target that links every engine module, for the game and tools.
vxe_add_engine_target()

if(VXE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
