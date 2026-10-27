#include "vxe/rhi/validation.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace vxe::rhi {
namespace {

std::string_view nameOr(std::string_view name) {
    return name.empty() ? std::string_view("unnamed") : name;
}

Error invalid(std::string_view what, std::string_view name, std::string_view problem) {
    return Error(ErrorCode::InvalidArgument, std::format("{} '{}': {}", what, nameOr(name), problem));
}

} // namespace

u32 fullMipCount(u32 width, u32 height) noexcept {
    const u32 largest = std::max({width, height, 1U});
    return static_cast<u32>(std::bit_width(largest));
}

usize textureLevelBytes(const TextureDesc& desc, u32 mipLevel) noexcept {
    const u32 width = std::max(desc.width >> mipLevel, 1U);
    const u32 height = std::max(desc.height >> mipLevel, 1U);
    return static_cast<usize>(width) * height * bytesPerTexel(desc.format);
}

usize textureTotalBytes(const TextureDesc& desc) noexcept {
    usize total = 0;
    for (u32 level = 0; level < desc.mipLevels; ++level) {
        total += textureLevelBytes(desc, level);
    }
    return total * desc.layers;
}

namespace {

Result<void> validateBufferRange(std::string_view action, usize bufferSize, std::string_view name,
                                 usize offset, usize size) {
    // Two comparisons, so a huge offset cannot overflow offset + size.
    if (offset > bufferSize || size > bufferSize - offset) {
        return invalid("Buffer", name,
                       std::format("{} of {} bytes at offset {} goes past the end ({} bytes)", action, size,
                                   offset, bufferSize));
    }
    return {};
}

} // namespace

Result<void> validateBufferWrite(usize bufferSize, BufferUsage usage, std::string_view name, usize offset,
                                 usize size) {
    if (!hasFlag(usage, BufferUsage::TransferDst)) {
        return invalid("Buffer", name, "CPU writes need BufferUsage::TransferDst");
    }
    return validateBufferRange("write", bufferSize, name, offset, size);
}

Result<void> validateBufferRead(usize bufferSize, BufferUsage usage, std::string_view name, usize offset,
                                usize size) {
    if (!hasFlag(usage, BufferUsage::TransferSrc)) {
        return invalid("Buffer", name, "reading back needs BufferUsage::TransferSrc");
    }
    return validateBufferRange("read", bufferSize, name, offset, size);
}

Result<void> validateTextureUpload(const TextureDesc& desc, std::string_view name,
                                   const TextureUpload& upload) {
    if (!hasFlag(desc.usage, TextureUsage::TransferDst)) {
        return invalid("Texture", name, "uploads need TextureUsage::TransferDst");
    }
    if (upload.mipLevel >= desc.mipLevels || upload.layer >= desc.layers) {
        return invalid("Texture", name,
                       std::format("upload names mip {} layer {}, but the texture has {} mips and {} layers",
                                   upload.mipLevel, upload.layer, desc.mipLevels, desc.layers));
    }
    const usize expected = textureLevelBytes(desc, upload.mipLevel);
    if (upload.data.size() != expected) {
        return invalid("Texture", name,
                       std::format("mip {} needs exactly {} bytes, got {}", upload.mipLevel, expected,
                                   upload.data.size()));
    }
    return {};
}

Result<void> validate(const BufferDesc& desc) {
    if (desc.sizeBytes == 0) {
        return invalid("Buffer", desc.debugName, "size is 0");
    }
    if (desc.usage == BufferUsage::None) {
        return invalid("Buffer", desc.debugName, "no usage flags; say what the buffer is for");
    }
    return {};
}

Result<void> validate(const TextureDesc& desc) {
    if (desc.format == Format::Undefined) {
        return invalid("Texture", desc.debugName, "format is Undefined");
    }
    if (desc.width == 0 || desc.height == 0) {
        return invalid("Texture", desc.debugName, "width and height must be at least 1");
    }
    if (desc.layers == 0 || (desc.type == TextureType::Texture2D && desc.layers != 1)) {
        return invalid("Texture", desc.debugName,
                       "a 2D texture has exactly 1 layer; use Texture2DArray for more");
    }
    if (desc.mipLevels == 0 || desc.mipLevels > fullMipCount(desc.width, desc.height)) {
        return invalid("Texture", desc.debugName,
                       std::format("mip levels must be 1 to {}", fullMipCount(desc.width, desc.height)));
    }
    if (desc.usage == TextureUsage::None) {
        return invalid("Texture", desc.debugName, "no usage flags");
    }
    if (hasFlag(desc.usage, TextureUsage::DepthTarget) != isDepthFormat(desc.format)) {
        return invalid("Texture", desc.debugName,
                       "depth targets need a depth format, and depth formats are only for depth targets");
    }
    if (hasFlag(desc.usage, TextureUsage::RenderTarget) && isDepthFormat(desc.format)) {
        return invalid("Texture", desc.debugName, "a depth format cannot be a color render target");
    }
    return {};
}

Result<void> validate(const SamplerDesc& desc) {
    if (desc.maxAnisotropy < 1.0F) {
        return invalid("Sampler", desc.debugName, "max anisotropy must be at least 1 (1 means off)");
    }
    return {};
}

Result<void> validate(const ShaderDesc& desc) {
    if (desc.source.empty()) {
        return invalid("Shader", desc.debugName, "source is empty");
    }
    return {};
}

Result<void> validate(const PipelineDesc& desc) {
    const bool compute = desc.compute.isValid();
    if (compute && (desc.vertex.isValid() || desc.fragment.isValid())) {
        return invalid("Pipeline", desc.debugName,
                       "a compute pipeline cannot also have vertex or fragment shaders");
    }
    if (!compute && !desc.vertex.isValid()) {
        return invalid("Pipeline", desc.debugName, "needs a vertex shader, or a compute shader");
    }
    if (desc.colorCount > kMaxColorAttachments) {
        return invalid("Pipeline", desc.debugName,
                       std::format("at most {} color targets", kMaxColorAttachments));
    }
    for (u32 i = 0; i < desc.colorCount; ++i) {
        if (desc.colorFormats[i] == Format::Undefined || isDepthFormat(desc.colorFormats[i])) {
            return invalid("Pipeline", desc.debugName,
                           std::format("color target {} needs a color format", i));
        }
    }
    if (desc.depthFormat != Format::Undefined && !isDepthFormat(desc.depthFormat)) {
        return invalid("Pipeline", desc.debugName, "depth format is not a depth format");
    }
    if (desc.depth.testEnabled && desc.depthFormat == Format::Undefined && !compute) {
        return invalid("Pipeline", desc.debugName,
                       "depth testing needs a depth format (or disable the depth test)");
    }
    return {};
}

} // namespace vxe::rhi
