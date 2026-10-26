# Compiler warning settings shared by every target we own.
# Third party code from vcpkg is never passed through this, so its warnings
# cannot break our build.
#
# Warnings are errors only when VXE_WARNINGS_AS_ERRORS is ON. CI turns it on,
# local builds keep it off so a new compiler version cannot block day to day work.

option(VXE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Applies the project warning level to one target.
# Usage: vxe_set_warnings(my_target)
function(vxe_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-          # Strict standard conformance.
            /utf-8                # Source and execution character sets are UTF-8.
            /Zc:__cplusplus       # Report the real C++ version in __cplusplus.
            /Zc:preprocessor      # Standard conforming preprocessor.
            /w14265               # Class has virtual functions but a non virtual destructor.
            /w14062               # Enumerator not handled in a switch.
        )
        if(VXE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Woverloaded-virtual
            -Wnull-dereference
            -Wdouble-promotion
            -Wimplicit-fallthrough
        )
        if(VXE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
