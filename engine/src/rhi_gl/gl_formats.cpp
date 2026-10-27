#include "gl_formats.hpp"

namespace vxe::rhi::gl {

// A three column table reads best positionally: internal format, CPU format, CPU type.
// NOLINTBEGIN(modernize-use-designated-initializers)
FormatInfo formatInfo(Format format) noexcept {
    switch (format) {
    case Format::Undefined:
        return {};
    case Format::R8Unorm:
        return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
    case Format::RG8Unorm:
        return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
    case Format::RGBA8Unorm:
        return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::RGBA8Srgb:
        return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE};
    case Format::BGRA8Unorm:
        // Stored as RGBA; only the CPU side layout differs.
        return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE};
    case Format::R16Float:
        return {GL_R16F, GL_RED, GL_HALF_FLOAT};
    case Format::RG16Float:
        return {GL_RG16F, GL_RG, GL_HALF_FLOAT};
    case Format::RGBA16Float:
        return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
    case Format::R32Float:
        return {GL_R32F, GL_RED, GL_FLOAT};
    case Format::RG32Float:
        return {GL_RG32F, GL_RG, GL_FLOAT};
    case Format::RGBA32Float:
        return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
    case Format::R32Uint:
        return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT};
    case Format::RG32Uint:
        return {GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT};
    case Format::RGBA32Uint:
        return {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT};
    case Format::R11G11B10Float:
        return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV};
    case Format::D32Float:
        return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
    case Format::D24UnormS8Uint:
        return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
    }
    return {};
}
// NOLINTEND(modernize-use-designated-initializers)

GLenum textureTarget(TextureType type) noexcept {
    return type == TextureType::Texture2DArray ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
}

GLenum minFilter(Filter filter, MipmapMode mipmapMode) noexcept {
    const bool linear = filter == Filter::Linear;
    switch (mipmapMode) {
    case MipmapMode::None:
        return linear ? GL_LINEAR : GL_NEAREST;
    case MipmapMode::Nearest:
        return linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    case MipmapMode::Linear:
        return linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    }
    return GL_LINEAR;
}

GLenum magFilter(Filter filter) noexcept {
    return filter == Filter::Linear ? GL_LINEAR : GL_NEAREST;
}

GLenum addressMode(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Repeat:
        return GL_REPEAT;
    case AddressMode::ClampToEdge:
        return GL_CLAMP_TO_EDGE;
    case AddressMode::MirroredRepeat:
        return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

} // namespace vxe::rhi::gl
