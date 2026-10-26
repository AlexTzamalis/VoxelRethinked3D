# Sanitizer support, selected with the VXE_SANITIZER cache variable:
#   none     No sanitizer (default).
#   address  AddressSanitizer. On Clang and GCC also UndefinedBehaviorSanitizer.
#   thread   ThreadSanitizer. Clang and GCC only, MSVC has no ThreadSanitizer.
#
# The flags are added globally (not per target) because a sanitizer only works
# reliably when every translation unit we compile is instrumented the same way.

set(VXE_SANITIZER "none" CACHE STRING "Sanitizer to build with: none, address, thread")
set_property(CACHE VXE_SANITIZER PROPERTY STRINGS none address thread)

if(VXE_SANITIZER STREQUAL "none")
    return()
endif()

if(NOT VXE_SANITIZER MATCHES "^(address|thread)$")
    message(FATAL_ERROR "Unknown VXE_SANITIZER value '${VXE_SANITIZER}'. Use none, address or thread.")
endif()

if(MSVC)
    if(VXE_SANITIZER STREQUAL "thread")
        message(FATAL_ERROR "MSVC does not support ThreadSanitizer. Use the tsan preset on Linux.")
    endif()

    add_compile_options(/fsanitize=address)
    # ASan cannot be combined with incremental linking.
    add_link_options(/INCREMENTAL:NO)
    # ASan cannot be combined with the runtime checks (/RTC1) that CMake adds to
    # Debug builds by default, so remove them from every config.
    foreach(flagVar CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/RTC[1csu]+" "" ${flagVar} "${${flagVar}}")
    endforeach()

    # MSVC links ASan against a runtime DLL that lives next to cl.exe. Outside a
    # Visual Studio developer shell (CLion, Explorer) it is not on PATH, so the
    # executables would fail to start. Copy it into the shared bin folder.
    get_filename_component(vxeCompilerDir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(vxeAsanDll "${vxeCompilerDir}/clang_rt.asan_dynamic-x86_64.dll")
    if(EXISTS "${vxeAsanDll}")
        file(COPY "${vxeAsanDll}" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    else()
        message(WARNING "ASan runtime not found at ${vxeAsanDll}. Run ASan builds from a VS developer shell.")
    endif()
else()
    if(VXE_SANITIZER STREQUAL "address")
        # By default UBSan only prints and keeps running, so a test would still
        # pass. Not recovering turns every undefined behavior report into a failure.
        set(vxeSanitizerFlags -fsanitize=address,undefined -fno-sanitize-recover=undefined)
    else()
        set(vxeSanitizerFlags -fsanitize=thread)
    endif()
    # Frame pointers give the sanitizer reports readable stack traces.
    add_compile_options(${vxeSanitizerFlags} -fno-omit-frame-pointer)
    add_link_options(${vxeSanitizerFlags})
endif()

message(STATUS "vxe: building with sanitizer '${VXE_SANITIZER}'")
