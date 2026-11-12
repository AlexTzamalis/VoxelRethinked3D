#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <span>
#include <vector>

namespace vxe::render {

// A picture of the window (Renderer::requestScreenshot): RGBA8, top row first.
struct Screenshot {
    u32 width = 0;
    u32 height = 0;
    std::vector<std::byte> rgba;
};

// The PNG file of a screenshot, alpha made opaque. Any thread.
[[nodiscard]] Result<std::vector<std::byte>> encodePng(const Screenshot& shot);

} // namespace vxe::render
