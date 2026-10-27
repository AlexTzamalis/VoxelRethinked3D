# The glad2 OpenGL 4.6 core loader, generated once and committed (see README.md).
#
# Only the OpenGL backend (rhi_gl) may link this, and only PRIVATE, so no other
# engine module can see GL headers (ADR-003, enforced by cmake/VxeModule.cmake).
#
# The generated loader is C, but it also compiles as C++, so it is built as
# C++ to keep the whole project on one language and one compiler setup.

add_library(vxe_glad STATIC
    include/glad/gl.h
    include/KHR/khrplatform.h
    src/gl.c
)
add_library(glad::glad ALIAS vxe_glad)
set_source_files_properties(src/gl.c PROPERTIES LANGUAGE CXX)
# SYSTEM, so warnings inside the generated header never fail rhi_gl's build.
target_include_directories(vxe_glad SYSTEM PUBLIC include)
