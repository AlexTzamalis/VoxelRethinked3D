# vcpkg triplet for the asan preset on Windows: the same as x64-windows-static-md,
# but every package is compiled with AddressSanitizer too.
#
# MSVC's standard library annotates containers for ASan and records that choice
# in every object file. Linking ASan code against libraries built without ASan
# therefore fails (LNK2038 "annotate_string" mismatch). Building the packages
# with ASan as well fixes that and also finds bugs inside them.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_C_FLAGS "/fsanitize=address")
set(VCPKG_CXX_FLAGS "/fsanitize=address")

# vcpkg picks its own Visual Studio instance, which may lack the ASan libraries
# (for example a Build Tools install without that component). Use the instance
# of the current developer shell instead, which is the one building our code.
# vcpkg matches the path literally against its instance list, which uses
# backslashes and no trailing separator. VSINSTALLDIR ends with a backslash.
if(DEFINED ENV{VSINSTALLDIR})
    string(REGEX REPLACE "[/\\]+$" "" vxeVisualStudioPath "$ENV{VSINSTALLDIR}")
    set(VCPKG_VISUAL_STUDIO_PATH "${vxeVisualStudioPath}")
endif()
