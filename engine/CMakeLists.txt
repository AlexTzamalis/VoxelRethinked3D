# The vxe engine. Each module becomes its own static library target.
# For now only vxe_core exists; later modules are added per the roadmap.

add_library(vxe_core STATIC
    src/core/version.cpp
)
add_library(vxe::core ALIAS vxe_core)

target_include_directories(vxe_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(vxe_core PUBLIC cxx_std_20)
target_compile_definitions(vxe_core PRIVATE
    VXE_VERSION_STRING="${PROJECT_VERSION}"
)
vxe_set_warnings(vxe_core)

if(VXE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
