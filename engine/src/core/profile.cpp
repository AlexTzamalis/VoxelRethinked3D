#include "vxe/core/profile.hpp"

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace vxe::profiling {

void setThreadName(const char* name) noexcept {
#if VXE_ENABLE_PROFILING
    tracy::SetThreadName(name);
#endif

#ifdef _WIN32
    // SetThreadDescription takes a wide string. Thread names are ASCII, so a
    // plain widening copy is enough. The allocation happens once per thread.
    try {
        const std::string narrow(name);
        const std::wstring wide(narrow.begin(), narrow.end());
        static_cast<void>(SetThreadDescription(GetCurrentThread(), wide.c_str()));
    } catch (...) {
        // Naming a thread is a debugging aid. Failing to allocate is not a reason to stop.
        return;
    }
#else
    // Linux limits thread names to 15 characters plus the terminator.
    char shortName[16] = {};
    std::string(name).copy(shortName, sizeof(shortName) - 1);
    static_cast<void>(pthread_setname_np(pthread_self(), shortName));
#endif
}

} // namespace vxe::profiling
