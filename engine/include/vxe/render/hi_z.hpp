#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <memory>

namespace vxe::render {

struct HiZPyramidDesc {
    rhi::Device* device = nullptr;
    rhi::ShaderFileReader shaders{};
};

// A depth pyramid for occlusion culling (the "Hi-Z build" pass of Section
// 8.1): a mipmapped r32f texture in which every texel holds the farthest
// depth of the depth buffer pixels it covers. Next frame's culling tests a
// chunk's screen rectangle against the level where that rectangle spans at
// most 2 x 2 texels: if the chunk's nearest point is farther than the
// farthest depth there, something drawn last frame hides all of it.
//
// Reversed Z (ADR-010) makes "farthest" the smallest value, so the pyramid
// keeps minimums.
//
// Level 0 is half the depth buffer's resolution. Texel t of level L covers
// depth pixels t * 2^(L+1) up to (t + 1) * 2^(L+1), clamped to the buffer;
// that exact power of two mapping holds for any buffer size because the
// texture is allocated with power of two levels and only the part that
// covers the buffer (validWidth, validHeight) is written. 1920 x 1080 needs
// a 1024 x 1024 base level with 11 levels, 5.6 MB.
//
// Thread safety: main thread only, like the device.
class HiZPyramid {
public:
    static constexpr u32 kMaxLevels = 16;

    [[nodiscard]] static Result<std::unique_ptr<HiZPyramid>> create(const HiZPyramidDesc& desc);
    ~HiZPyramid();
    HiZPyramid(const HiZPyramid&) = delete;
    HiZPyramid& operator=(const HiZPyramid&) = delete;
    HiZPyramid(HiZPyramid&&) = delete;
    HiZPyramid& operator=(HiZPyramid&&) = delete;

    // Makes the pyramid fit a depth buffer of this size, recreating the
    // texture if needed (its contents are then gone until the next record).
    // Call before recording anything of the frame: a command list that
    // already uses the old texture would find it destroyed.
    [[nodiscard]] Result<void> ensureSize(u32 width, u32 height);
    // Records the reduction of `depth` (a Sampled D32Float texture of the
    // size given to ensureSize) into the pyramid. Record outside render
    // passes, after everything that writes depth. Ends with a barrier, so
    // later texture fetches see the result.
    void record(rhi::CommandList& commands, rhi::TextureHandle depth);

    // The pyramid texture and a nearest sampler for texelFetch. Before the
    // first record() it is a 1 x 1 placeholder and hasData() is false.
    [[nodiscard]] rhi::TextureHandle texture() const noexcept { return m_texture; }
    [[nodiscard]] rhi::SamplerHandle sampler() const noexcept { return m_sampler; }
    [[nodiscard]] bool hasData() const noexcept { return m_hasData; }
    [[nodiscard]] u32 levels() const noexcept { return m_levels; }
    // The size of the depth buffer the pyramid was built from, in pixels.
    [[nodiscard]] u32 depthWidth() const noexcept { return m_depthWidth; }
    [[nodiscard]] u32 depthHeight() const noexcept { return m_depthHeight; }
    [[nodiscard]] usize memoryBytes() const noexcept;

    // The number of texels of a level that cover a depth buffer `pixels`
    // wide: ceil(pixels / 2^(level + 1)).
    [[nodiscard]] static constexpr u32 validTexels(u32 pixels, u32 level) noexcept {
        const u32 shift = level + 1;
        return (pixels + (1U << shift) - 1U) >> shift;
    }

private:
    explicit HiZPyramid(rhi::Device& device) noexcept
        : m_device(device) {}
    [[nodiscard]] Result<void> resize(u32 width, u32 height);

    rhi::Device& m_device;
    rhi::PipelineHandle m_fromDepth;
    rhi::PipelineHandle m_reduce;
    rhi::TextureHandle m_texture;
    rhi::SamplerHandle m_sampler;
    rhi::SamplerHandle m_depthSampler;
    rhi::BufferHandle m_params; // One 256 byte aligned block per level.
    u32 m_paramStride = 256;
    u32 m_levels = 1;
    u32 m_baseWidth = 1;
    u32 m_baseHeight = 1;
    u32 m_depthWidth = 0;
    u32 m_depthHeight = 0;
    bool m_hasData = false;
};

} // namespace vxe::render
