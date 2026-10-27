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

// The byte size of one mip level of one layer, for upload checks.
[[nodiscard]] usize textureLevelBytes(const TextureDesc& desc, u32 mipLevel) noexcept;

// The number of mip levels a full chain of a width by height texture has.
[[nodiscard]] u32 fullMipCount(u32 width, u32 height) noexcept;

} // namespace vxe::rhi
