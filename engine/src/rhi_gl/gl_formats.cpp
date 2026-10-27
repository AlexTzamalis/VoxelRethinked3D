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

GLenum shaderStage(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return GL_VERTEX_SHADER;
    case ShaderStage::Fragment:
        return GL_FRAGMENT_SHADER;
    case ShaderStage::Compute:
        return GL_COMPUTE_SHADER;
    }
    return GL_VERTEX_SHADER;
}

GLenum primitiveTopology(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::Triangles:
        return GL_TRIANGLES;
    case PrimitiveTopology::Lines:
        return GL_LINES;
    case PrimitiveTopology::Points:
        return GL_POINTS;
    }
    return GL_TRIANGLES;
}

GLenum polygonMode(PolygonMode mode) noexcept {
    return mode == PolygonMode::Line ? GL_LINE : GL_FILL;
}

GLenum frontFace(FrontFace face) noexcept {
    return face == FrontFace::Clockwise ? GL_CW : GL_CCW;
}

GLenum compareOp(CompareOp op) noexcept {
    switch (op) {
    case CompareOp::Never:
        return GL_NEVER;
    case CompareOp::Less:
        return GL_LESS;
    case CompareOp::LessOrEqual:
        return GL_LEQUAL;
    case CompareOp::Equal:
        return GL_EQUAL;
    case CompareOp::GreaterOrEqual:
        return GL_GEQUAL;
    case CompareOp::Greater:
        return GL_GREATER;
    case CompareOp::NotEqual:
        return GL_NOTEQUAL;
    case CompareOp::Always:
        return GL_ALWAYS;
    }
    return GL_ALWAYS;
}

GLenum blendFactor(BlendFactor factor) noexcept {
    switch (factor) {
    case BlendFactor::Zero:
        return GL_ZERO;
    case BlendFactor::One:
        return GL_ONE;
    case BlendFactor::SrcColor:
        return GL_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha:
        return GL_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstColor:
        return GL_DST_COLOR;
    case BlendFactor::OneMinusDstColor:
        return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::DstAlpha:
        return GL_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    }
    return GL_ONE;
}

GLenum blendOp(BlendOp op) noexcept {
    switch (op) {
    case BlendOp::Add:
        return GL_FUNC_ADD;
    case BlendOp::Subtract:
        return GL_FUNC_SUBTRACT;
    case BlendOp::ReverseSubtract:
        return GL_FUNC_REVERSE_SUBTRACT;
    case BlendOp::Min:
        return GL_MIN;
    case BlendOp::Max:
        return GL_MAX;
    }
    return GL_FUNC_ADD;
}

} // namespace vxe::rhi::gl
