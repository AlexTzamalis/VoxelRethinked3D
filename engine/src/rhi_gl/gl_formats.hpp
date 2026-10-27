#pragma once

// Translation of RHI enums into OpenGL enums. Private to the OpenGL backend.

#include <vxe/rhi/types.hpp>

#include <glad/gl.h>

namespace vxe::rhi::gl {

// The three enums OpenGL needs for a texture format: how it is stored on the
// GPU (internal format) and how CPU data for it is laid out (format, type).
struct FormatInfo {
    GLenum internalFormat = 0;
    GLenum format = 0;
    GLenum type = 0;
};

[[nodiscard]] FormatInfo formatInfo(Format format) noexcept;
[[nodiscard]] GLenum textureTarget(TextureType type) noexcept;
[[nodiscard]] GLenum minFilter(Filter filter, MipmapMode mipmapMode) noexcept;
[[nodiscard]] GLenum magFilter(Filter filter) noexcept;
[[nodiscard]] GLenum addressMode(AddressMode mode) noexcept;

} // namespace vxe::rhi::gl
