# The vxe engine. Each module is its own static library created with
# vxe_add_module() (cmake/VxeModule.cmake), which enforces the layer order.
# Add modules from the lowest layer up, in the order of VXE_MODULE_LAYERS.

set(VXE_ENGINE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

vxe_add_module(core
    SOURCES
        src/core/version.cpp
)
target_compile_definitions(vxe_core PRIVATE
    VXE_VERSION_STRING="${PROJECT_VERSION}"
)

# One target that links every engine module, for the game and tools.
vxe_add_engine_target()

if(VXE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
