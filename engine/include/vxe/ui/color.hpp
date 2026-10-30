#pragma once

#include <vxe/core/types.hpp>

namespace vxe::ui {

// A color as the window shows it: red in the lowest byte, alpha in the
// highest (the byte order unpackUnorm4x8 reads in the shader).
[[nodiscard]] constexpr u32 rgba(u8 red, u8 green, u8 blue, u8 alpha = 255) noexcept {
    return u32{red} | (u32{green} << 8U) | (u32{blue} << 16U) | (u32{alpha} << 24U);
}

} // namespace vxe::ui
