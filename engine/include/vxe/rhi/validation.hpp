#pragma once

#include <vxe/core/result.hpp>
#include <vxe/rhi/types.hpp>

// Checks of resource descriptions before a backend creates anything
// (Section 7, E6.1). A description that can never work (a zero sized buffer,
// a depth format used as a color target) fails here with a clear message
// instead of as a driver error or a black screen. Backends call these in
// every build; the cost is a few comparisons per resource creation.
namespace vxe::rhi {

[[nodiscard]] Result<void> validate(const BufferDesc& desc);
[[nodiscard]] Result<void> validate(const TextureDesc& desc);
[[nodiscard]] Result<void> validate(const SamplerDesc& desc);
[[nodiscard]] Result<void> validate(const ShaderDesc& desc);
[[nodiscard]] Result<void> validate(const PipelineDesc& desc);

// Checks a CPU write into a buffer (Device::updateBuffer) or a read back
// (Device::readBuffer). Writes need BufferUsage::TransferDst and reads
// TransferSrc, the same rule Vulkan applies, so code written against one
// backend never breaks on another. `name` is only used in the message.
[[nodiscard]] Result<void> validateBufferWrite(usize bufferSize, BufferUsage usage, std::string_view name,
                                               usize offset, usize size);
[[nodiscard]] Result<void> validateBufferRead(usize bufferSize, BufferUsage usage, std::string_view name,
                                              usize offset, usize size);

// Checks one texture upload: the texture allows TransferDst, the mip level
// and layer exist, and the data holds exactly one tightly packed level.
[[nodiscard]] Result<void> validateTextureUpload(const TextureDesc& desc, std::string_view name,
                                                 const TextureUpload& upload);

// Checks one texture read back: TransferSrc, an existing mip level and layer,
// and an output of exactly one tightly packed level.
[[nodiscard]] Result<void> validateTextureRead(const TextureDesc& desc, std::string_view name, u32 mipLevel,
                                               u32 layer, usize outSize);

// The byte size of one mip level of one layer, for upload checks.
[[nodiscard]] usize textureLevelBytes(const TextureDesc& desc, u32 mipLevel) noexcept;

// The byte size of the whole texture: every mip level of every layer.
[[nodiscard]] usize textureTotalBytes(const TextureDesc& desc) noexcept;

// The number of mip levels a full chain of a width by height texture has.
[[nodiscard]] u32 fullMipCount(u32 width, u32 height) noexcept;

} // namespace vxe::rhi
