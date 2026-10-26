#pragma once

#include <string_view>

namespace vxe {

// Returns the engine version as "major.minor.patch", for example "0.1.0".
// The value comes from the project version in the root CMakeLists.txt, so it
// is defined in exactly one place. The string has static storage duration and
// stays valid for the whole program.
[[nodiscard]] std::string_view engineVersion() noexcept;

} // namespace vxe
