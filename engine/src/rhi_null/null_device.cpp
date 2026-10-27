#include "vxe/rhi_null/null_device.hpp"

#include <vxe/core/log.hpp>
#include <vxe/rhi/validation.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace vxe::rhi {
namespace {

struct NullBuffer {
    usize size = 0;
    BufferUsage usage = BufferUsage::None;
    std::string name;
    // Real memory, so mapped staging ranges and updates behave like the GPU
    // version from the caller's point of view.
    std::vector<std::byte> memory;
};

struct NullTexture {
    TextureDesc desc; // debugName cleared; `name` owns the text.
    std::string name;
    usize bytes = 0;
};

struct NullSampler {};

struct NullShader {
    ShaderStage stage = ShaderStage::Vertex;
    std::string name;
};

struct NullPipeline {
    bool compute = false;
    std::string name;
};

struct NullFence {};

// A Device without a GPU (Section 7.6). Everything a real backend checks is
// checked here too, so code tested against the Null device fails the same
// way it would on the GPU.
class NullDevice final : public Device {
public:
    NullDevice() {
        m_capabilities.backend = Backend::Null;
        m_capabilities.rendererName = "Null device";
        m_capabilities.maxTextureArrayLayers = 2048;
        m_capabilities.maxTextureSize = 16384;
        m_capabilities.maxComputeWorkGroupInvocations = 1024;
    }

    NullDevice(const NullDevice&) = delete;
    NullDevice& operator=(const NullDevice&) = delete;
    NullDevice(NullDevice&&) = delete;
    NullDevice& operator=(NullDevice&&) = delete;

    // Reporting leaks allocates; failing at shutdown cannot be recovered from, so
    // terminating (what an escaping exception does here) is acceptable.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~NullDevice() override { reportLeaks(); }

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override { return m_capabilities; }

    [[nodiscard]] DeviceStats stats() const override {
        DeviceStats stats;
        m_buffers.forEach(
            [&stats](BufferHandle, const NullBuffer& buffer) { stats.bufferBytes += buffer.size; });
        m_textures.forEach(
            [&stats](TextureHandle, const NullTexture& texture) { stats.textureBytes += texture.bytes; });
        stats.buffers = static_cast<u32>(m_buffers.liveCount());
        stats.textures = static_cast<u32>(m_textures.liveCount());
        stats.samplers = static_cast<u32>(m_samplers.liveCount());
        stats.shaders = static_cast<u32>(m_shaders.liveCount());
        stats.pipelines = static_cast<u32>(m_pipelines.liveCount());
        stats.commandsSubmitted = m_commandsSubmitted;
        stats.invalidCommands = m_invalidCommands;
        stats.framesPresented = m_framesPresented;
        return stats;
    }

    Result<BufferHandle> createBuffer(const BufferDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_buffers.allocate(NullBuffer{.size = desc.sizeBytes,
                                             .usage = desc.usage,
                                             .name = std::string(desc.debugName),
                                             .memory = std::vector<std::byte>(desc.sizeBytes)});
    }

    Result<TextureHandle> createTexture(const TextureDesc& desc) override {
        VXE_TRY(validate(desc));
        if (desc.width > m_capabilities.maxTextureSize || desc.height > m_capabilities.maxTextureSize ||
            desc.layers > m_capabilities.maxTextureArrayLayers) {
            return Error(ErrorCode::Unsupported,
                         std::format("Texture '{}' exceeds the device limits", desc.debugName));
        }
        TextureDesc stored = desc;
        stored.debugName = {};
        return m_textures.allocate(NullTexture{
            .desc = stored, .name = std::string(desc.debugName), .bytes = textureTotalBytes(desc)});
    }

    Result<SamplerHandle> createSampler(const SamplerDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_samplers.allocate(NullSampler{});
    }

    Result<ShaderHandle> createShader(const ShaderDesc& desc) override {
        VXE_TRY(validate(desc));
        return m_shaders.allocate(NullShader{.stage = desc.stage, .name = std::string(desc.debugName)});
    }

    Result<PipelineHandle> createPipeline(const PipelineDesc& desc) override {
        VXE_TRY(validate(desc));
        // The shaders must exist and sit in the right stage, as on the GPU.
        const auto checkStage = [this, &desc](ShaderHandle handle, ShaderStage stage) -> Result<void> {
            if (!handle.isValid()) {
                return {};
            }
            const NullShader* shader = m_shaders.get(handle);
            if (shader == nullptr || shader->stage != stage) {
                return Error(ErrorCode::InvalidArgument,
                             std::format("Pipeline '{}' uses a missing shader or one of the wrong stage",
                                         desc.debugName));
            }
            return {};
        };
        VXE_TRY(checkStage(desc.vertex, ShaderStage::Vertex));
        VXE_TRY(checkStage(desc.fragment, ShaderStage::Fragment));
        VXE_TRY(checkStage(desc.compute, ShaderStage::Compute));
        return m_pipelines.allocate(
            NullPipeline{.compute = desc.compute.isValid(), .name = std::string(desc.debugName)});
    }

    void destroy(BufferHandle handle) override { static_cast<void>(m_buffers.release(handle)); }
    void destroy(TextureHandle handle) override { static_cast<void>(m_textures.release(handle)); }
    void destroy(SamplerHandle handle) override { static_cast<void>(m_samplers.release(handle)); }
    void destroy(ShaderHandle handle) override { static_cast<void>(m_shaders.release(handle)); }
    void destroy(PipelineHandle handle) override { static_cast<void>(m_pipelines.release(handle)); }

    Result<void> updateBuffer(BufferHandle handle, usize offset, std::span<const std::byte> data) override {
        NullBuffer* buffer = m_buffers.get(handle);
        if (buffer == nullptr) {
            return Error(ErrorCode::InvalidArgument, "updateBuffer on a destroyed or invalid buffer");
        }
        VXE_TRY(validateBufferWrite(buffer->size, buffer->usage, buffer->name, offset, data.size()));
        std::ranges::copy(data, buffer->memory.begin() + static_cast<std::ptrdiff_t>(offset));
        return {};
    }

    Result<void> readBuffer(BufferHandle handle, usize offset, std::span<std::byte> out) override {
        const NullBuffer* buffer = m_buffers.get(handle);
        if (buffer == nullptr) {
            return Error(ErrorCode::InvalidArgument, "readBuffer on a destroyed or invalid buffer");
        }
        VXE_TRY(validateBufferRead(buffer->size, buffer->usage, buffer->name, offset, out.size()));
        std::copy_n(buffer->memory.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return {};
    }

    Result<void> uploadTexture(TextureHandle handle, const TextureUpload& upload) override {
        const NullTexture* texture = m_textures.get(handle);
        if (texture == nullptr) {
            return Error(ErrorCode::InvalidArgument, "uploadTexture on a destroyed or invalid texture");
        }
        return validateTextureUpload(texture->desc, texture->name, upload);
    }

    std::span<std::byte> mappedRange(BufferHandle handle) override {
        NullBuffer* buffer = m_buffers.get(handle);
        if (buffer == nullptr || !hasFlag(buffer->usage, BufferUsage::CpuWrite)) {
            return {};
        }
        return buffer->memory;
    }

    FenceHandle insertFence() override { return m_fences.allocate(NullFence{}); }
    bool isFenceSignaled(FenceHandle fence) override {
        return m_fences.isLive(fence);
    } // No GPU: done at once.
    void destroy(FenceHandle fence) override { static_cast<void>(m_fences.release(fence)); }

    void submit(const CommandList& commands) override {
        bool insidePass = false;
        for (const CommandView command : commands) {
            ++m_commandsSubmitted;
            if (!isCommandValid(command, insidePass)) {
                ++m_invalidCommands;
            }
        }
        if (insidePass) {
            ++m_invalidCommands; // A render pass was never ended.
        }
    }

    void beginFrame() override {}
    void present() override { ++m_framesPresented; }
    void setPresentMode(PresentMode /*mode*/) override {}
    [[nodiscard]] u32 backBufferWidth() const noexcept override { return 0; }
    [[nodiscard]] u32 backBufferHeight() const noexcept override { return 0; }

private:
    // True if every handle the command uses is alive and the command is
    // allowed where it appears (draws only inside render passes).
    bool isCommandValid(const CommandView& command, bool& insidePass) const {
        switch (command.type) {
        case CommandType::BeginRenderPass: {
            const auto begin = command.as<cmd::BeginRenderPass>();
            const bool nested = insidePass;
            insidePass = true;
            for (u32 i = 0; i < begin.pass.colorCount; ++i) {
                if (!begin.pass.colors[i].swapchain && !m_textures.isLive(begin.pass.colors[i].texture)) {
                    return false;
                }
            }
            return !nested && (!begin.pass.hasDepth || m_textures.isLive(begin.pass.depth.texture));
        }
        case CommandType::EndRenderPass: {
            const bool wasInside = insidePass;
            insidePass = false;
            return wasInside;
        }
        case CommandType::BindPipeline:
            return m_pipelines.isLive(command.as<cmd::BindPipeline>().pipeline);
        case CommandType::BindUniformBuffer:
        case CommandType::BindStorageBuffer:
            return m_buffers.isLive(command.as<cmd::BindBufferRange>().buffer);
        case CommandType::BindTexture: {
            const auto bind = command.as<cmd::BindTexture>();
            return m_textures.isLive(bind.texture) && m_samplers.isLive(bind.sampler);
        }
        case CommandType::BindIndexBuffer:
            return m_buffers.isLive(command.as<cmd::BindIndexBuffer>().buffer);
        case CommandType::Draw:
        case CommandType::DrawIndexed:
            return insidePass;
        case CommandType::MultiDrawIndexedIndirect:
            return insidePass && m_buffers.isLive(command.as<cmd::MultiDrawIndexedIndirect>().commands);
        case CommandType::MultiDrawIndexedIndirectCount: {
            const auto draw = command.as<cmd::MultiDrawIndexedIndirectCount>();
            return insidePass && m_buffers.isLive(draw.commands) && m_buffers.isLive(draw.count);
        }
        case CommandType::Dispatch:
            return !insidePass; // Compute runs outside render passes.
        case CommandType::CopyBuffer: {
            const auto copy = command.as<cmd::CopyBuffer>();
            return !insidePass && m_buffers.isLive(copy.src) && m_buffers.isLive(copy.dst);
        }
        case CommandType::SetViewport:
        case CommandType::SetScissor:
        case CommandType::Barrier:
        case CommandType::BeginDebugGroup:
        case CommandType::EndDebugGroup:
        case CommandType::WriteTimestamp:
            return true;
        case CommandType::Count:
            break;
        }
        return false;
    }

    // Resources still alive when the device goes away are leaks; naming them
    // makes them easy to find (E6.6).
    void reportLeaks() const {
        std::vector<std::string> leaks;
        m_buffers.forEach(
            [&leaks](BufferHandle, const NullBuffer& b) { leaks.push_back("buffer '" + b.name + "'"); });
        m_textures.forEach(
            [&leaks](TextureHandle, const NullTexture& t) { leaks.push_back("texture '" + t.name + "'"); });
        m_shaders.forEach(
            [&leaks](ShaderHandle, const NullShader& s) { leaks.push_back("shader '" + s.name + "'"); });
        m_pipelines.forEach([&leaks](PipelineHandle, const NullPipeline& p) {
            leaks.push_back("pipeline '" + p.name + "'");
        });
        if (m_samplers.liveCount() > 0) {
            leaks.push_back(std::format("{} samplers", m_samplers.liveCount()));
        }
        for (const std::string& leak : leaks) {
            VXE_LOG_WARN(log::core, "Null device destroyed with a live {}", leak);
        }
    }

    DeviceCapabilities m_capabilities;
    HandlePool<BufferTag, NullBuffer> m_buffers;
    HandlePool<TextureTag, NullTexture> m_textures;
    HandlePool<SamplerTag, NullSampler> m_samplers;
    HandlePool<ShaderTag, NullShader> m_shaders;
    HandlePool<PipelineTag, NullPipeline> m_pipelines;
    HandlePool<FenceTag, NullFence> m_fences;
    u64 m_commandsSubmitted = 0;
    u64 m_invalidCommands = 0;
    u64 m_framesPresented = 0;
};

} // namespace

std::unique_ptr<Device> createNullDevice() {
    return std::make_unique<NullDevice>();
}

} // namespace vxe::rhi
