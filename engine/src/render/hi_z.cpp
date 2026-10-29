#include "vxe/render/hi_z.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/math/vector.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <vector>

namespace vxe::render {
namespace {

constexpr u32 kGroupSize = 8; // local_size of render/hiz_reduce.comp.

struct LevelParams {
    glm::ivec4 params{0}; // xy: depth size, z: level.
};

} // namespace

Result<std::unique_ptr<HiZPyramid>> HiZPyramid::create(const HiZPyramidDesc& desc) {
    VXE_ASSERT(desc.device != nullptr, "HiZPyramid needs a device");
    rhi::Device& device = *desc.device;
    std::unique_ptr<HiZPyramid> pyramid(new HiZPyramid(device));
    HiZPyramid& p = *pyramid;
    const auto fail = [](const Error& error) { return error.withContext("Depth pyramid"); };

    const std::array<std::vector<rhi::ShaderDefine>, 2> defines = {
        std::vector<rhi::ShaderDefine>{{.name = "VXE_HIZ_FROM_DEPTH"}}, std::vector<rhi::ShaderDefine>{}};
    const std::array<rhi::PipelineHandle*, 2> targets = {&p.m_fromDepth, &p.m_reduce};
    for (usize i = 0; i < targets.size(); ++i) {
        const auto shader =
            rhi::createShaderFromFile(device, "render/hiz_reduce.comp", desc.shaders, defines.at(i));
        if (!shader) {
            return fail(shader.error());
        }
        auto pipeline = device.createPipeline(
            {.compute = shader.value(),
             .debugName = i == 0 ? "Depth pyramid from depth" : "Depth pyramid reduce"});
        device.destroy(shader.value());
        if (!pipeline) {
            return fail(pipeline.error());
        }
        *targets.at(i) = pipeline.value();
    }
    p.m_paramStride = std::max<u32>(device.capabilities().uniformBufferOffsetAlignment,
                                    static_cast<u32>(sizeof(LevelParams)));
    auto params = device.createBuffer({.sizeBytes = usize{p.m_paramStride} * kMaxLevels,
                                       .usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::TransferDst,
                                       .debugName = "Depth pyramid levels"});
    if (!params) {
        return fail(params.error());
    }
    p.m_params = params.value();
    // texelFetch ignores filtering, but the texture must still be complete
    // for its mip levels, hence a mipmapped nearest sampler.
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::Nearest,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "Depth pyramid"});
    if (!sampler) {
        return fail(sampler.error());
    }
    p.m_sampler = sampler.value();
    auto depthSampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                              .magFilter = rhi::Filter::Nearest,
                                              .mipmapMode = rhi::MipmapMode::Nearest,
                                              .addressU = rhi::AddressMode::ClampToEdge,
                                              .addressV = rhi::AddressMode::ClampToEdge,
                                              .debugName = "Depth pyramid source"});
    if (!depthSampler) {
        return fail(depthSampler.error());
    }
    p.m_depthSampler = depthSampler.value();
    if (Result<void> placeholder = p.resize(1, 1); !placeholder) {
        return fail(placeholder.error());
    }
    p.m_depthWidth = 0;
    p.m_depthHeight = 0;
    return pyramid;
}

HiZPyramid::~HiZPyramid() {
    for (const rhi::PipelineHandle pipeline : {m_fromDepth, m_reduce}) {
        if (pipeline.isValid()) {
            m_device.destroy(pipeline);
        }
    }
    for (const rhi::SamplerHandle sampler : {m_sampler, m_depthSampler}) {
        if (sampler.isValid()) {
            m_device.destroy(sampler);
        }
    }
    if (m_texture.isValid()) {
        m_device.destroy(m_texture);
    }
    if (m_params.isValid()) {
        m_device.destroy(m_params);
    }
}

usize HiZPyramid::memoryBytes() const noexcept {
    usize bytes = 0;
    for (u32 level = 0; level < m_levels; ++level) {
        bytes += usize{std::max(m_baseWidth >> level, 1U)} * std::max(m_baseHeight >> level, 1U) * 4;
    }
    return bytes;
}

Result<void> HiZPyramid::resize(u32 width, u32 height) {
    // Power of two levels keep the texel to pixel mapping exact (see the
    // class comment); only the levels down to one valid texel are needed.
    const u32 baseWidth = std::bit_ceil(validTexels(width, 0));
    const u32 baseHeight = std::bit_ceil(validTexels(height, 0));
    u32 levels = 1;
    while (levels < kMaxLevels &&
           (validTexels(width, levels - 1) > 1 || validTexels(height, levels - 1) > 1)) {
        ++levels;
    }
    auto texture = m_device.createTexture(
        {.format = rhi::Format::R32Float,
         .width = baseWidth,
         .height = baseHeight,
         .mipLevels = levels,
         .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc,
         .debugName = "Depth pyramid"});
    if (!texture) {
        return texture.error();
    }
    // The device keeps the old texture alive until frames in flight are done.
    if (m_texture.isValid()) {
        m_device.destroy(m_texture);
    }
    m_texture = texture.value();
    m_baseWidth = baseWidth;
    m_baseHeight = baseHeight;
    m_levels = levels;
    m_depthWidth = width;
    m_depthHeight = height;
    m_hasData = false;
    std::vector<std::byte> blocks(usize{m_paramStride} * kMaxLevels);
    for (u32 level = 0; level < levels; ++level) {
        const LevelParams params{.params = glm::ivec4(static_cast<i32>(width), static_cast<i32>(height),
                                                      static_cast<i32>(level), 0)};
        std::memcpy(blocks.data() + (usize{level} * m_paramStride), &params, sizeof(params));
    }
    return m_device.updateBuffer(m_params, 0, blocks);
}

Result<void> HiZPyramid::ensureSize(u32 width, u32 height) {
    if (width == 0 || height == 0 || (width == m_depthWidth && height == m_depthHeight)) {
        return {};
    }
    return resize(width, height);
}

void HiZPyramid::record(rhi::CommandList& commands, rhi::TextureHandle depth) {
    VXE_PROFILE_FUNCTION();
    const u32 width = m_depthWidth;
    const u32 height = m_depthHeight;
    if (width == 0 || height == 0) {
        return;
    }
    for (u32 level = 0; level < m_levels; ++level) {
        commands.bindPipeline(level == 0 ? m_fromDepth : m_reduce);
        commands.bindUniformBuffer(1, 0, m_params, usize{level} * m_paramStride, sizeof(LevelParams));
        if (level == 0) {
            commands.bindTexture(2, 0, depth, m_depthSampler);
        } else {
            commands.bindStorageImage(1, 0, m_texture, level - 1, rhi::ImageAccess::ReadOnly);
        }
        commands.bindStorageImage(1, 1, m_texture, level, rhi::ImageAccess::WriteOnly);
        commands.dispatch((validTexels(width, level) + kGroupSize - 1) / kGroupSize,
                          (validTexels(height, level) + kGroupSize - 1) / kGroupSize);
        commands.resourceBarrier(rhi::BarrierScope::StorageImage);
    }
    commands.resourceBarrier(rhi::BarrierScope::StorageImage | rhi::BarrierScope::TextureFetch);
    m_hasData = true;
}

} // namespace vxe::render
