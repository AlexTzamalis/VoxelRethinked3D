#include "vxe/core/version.hpp"

namespace vxe {

std::string_view engineVersion() noexcept {
    // VXE_VERSION_STRING is set by engine/CMakeLists.txt from PROJECT_VERSION.
    // Keeping it out of the public header means a version bump only recompiles
    // this one file instead of everything that includes the header.
    return VXE_VERSION_STRING;
}

} // namespace vxe
