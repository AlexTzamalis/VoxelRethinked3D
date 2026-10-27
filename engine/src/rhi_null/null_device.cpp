#include "vxe/rhi_null/null_device.hpp"

#include <vxe/rhi/validation.hpp>

#include <string>
#include <utility>

namespace vxe::rhi {
namespace {

struct NullBuffer {
    BufferDesc desc;
    std::string name;
};

struct NullTexture {
    TextureDesc desc;
    std::string name;
};

struct NullSampler {
    SamplerDesc desc;
};

struct NullShader {
    ShaderStage stage = ShaderStage::Vertex;
};

struct NullPipeline {
    PipelineDesc desc;
};

struct NullFence {};

// The skeleton of the Null device (E6.1): handle tables and validation.
// Mapped memory, statistics and command counting follow in E6.2.
class NullDevice final : public Device {
public:
    NullDevice() { m_capabilities.backend = Backend::Null; }

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override { return m_capabilities; }
    [[nodiscard]] DeviceStats stats() const override { return {}; }

    Result<BufferHandle> createBuffer(const BufferDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_buffers.allocate(NullBuffer{.desc = desc, .name = std::string(desc.debugName)});
    }
    Result<TextureHandle> createTexture(const TextureDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_textures.allocate(NullTexture{.desc = desc, .name = std::string(desc.debugName)});
    }
    Result<SamplerHandle> createSampler(const SamplerDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_samplers.allocate(NullSampler{.desc = desc});
    }
    Result<ShaderHandle> createShader(const ShaderDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_shaders.allocate(NullShader{.stage = desc.stage});
    }
    Result<PipelineHandle> createPipeline(const PipelineDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_pipelines.allocate(NullPipeline{.desc = desc});
    }

    void destroy(BufferHandle handle) override { static_cast<void>(m_buffers.release(handle)); }
    void destroy(TextureHandle handle) override { static_cast<void>(m_textures.release(handle)); }
    void destroy(SamplerHandle handle) override { static_cast<void>(m_samplers.release(handle)); }
    void destroy(ShaderHandle handle) override { static_cast<void>(m_shaders.release(handle)); }
    void destroy(PipelineHandle handle) override { static_cast<void>(m_pipelines.release(handle)); }

    Result<void> updateBuffer(BufferHandle /*buffer*/, usize /*offset*/,
                              std::span<const std::byte> /*data*/) override {
        return {};
    }
    Result<void> uploadTexture(TextureHandle /*texture*/, const TextureUpload& /*upload*/) override {
        return {};
    }
    std::span<std::byte> mappedRange(BufferHandle /*buffer*/) override { return {}; }

    FenceHandle insertFence() override { return m_fences.allocate(NullFence{}); }
    bool isFenceSignaled(FenceHandle /*fence*/) override { return true; }
    void destroy(FenceHandle fence) override { static_cast<void>(m_fences.release(fence)); }

    void submit(const CommandList& /*commands*/) override {}
    void beginFrame() override {}
    void present() override {}
    void setPresentMode(PresentMode /*mode*/) override {}
    [[nodiscard]] u32 backBufferWidth() const noexcept override { return 0; }
    [[nodiscard]] u32 backBufferHeight() const noexcept override { return 0; }

private:
    DeviceCapabilities m_capabilities;
    HandlePool<BufferTag, NullBuffer> m_buffers;
    HandlePool<TextureTag, NullTexture> m_textures;
    HandlePool<SamplerTag, NullSampler> m_samplers;
    HandlePool<ShaderTag, NullShader> m_shaders;
    HandlePool<PipelineTag, NullPipeline> m_pipelines;
    HandlePool<FenceTag, NullFence> m_fences;
};

} // namespace

std::unique_ptr<Device> createNullDevice() {
    return std::make_unique<NullDevice>();
}

} // namespace vxe::rhi
