# The toolchain file of every preset: loads vcpkg from the VCPKG_ROOT the user
# set. Visual Studio's developer environment replaces that variable with its
# own private vcpkg, so every tool would use a different vcpkg, and switching
# between them would install all packages again. The value saved for the user
# in the Windows registry survives that; the environment is the fallback (CI).
set(vxeVcpkgRoot "$ENV{VCPKG_ROOT}")
if(CMAKE_HOST_WIN32)
    cmake_host_system_information(RESULT vxeUserVcpkgRoot QUERY WINDOWS_REGISTRY "HKCU/Environment"
        VALUE VCPKG_ROOT ERROR_VARIABLE vxeRegistryError)
    if(vxeUserVcpkgRoot)
        set(vxeVcpkgRoot "${vxeUserVcpkgRoot}")
    endif()
endif()
if(NOT EXISTS "${vxeVcpkgRoot}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR "vcpkg not found at '${vxeVcpkgRoot}'. Set VCPKG_ROOT, see docs/building.md.")
endif()
include("${vxeVcpkgRoot}/scripts/buildsystems/vcpkg.cmake")
