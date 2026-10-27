#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/handles.hpp>
#include <vxe/rhi/types.hpp>

#include <span>
#include <string>

namespace vxe::rhi {

// What the hardware and driver support. The renderer reads this once at
// startup and picks fast paths; the debug overlay displays it (Section 18.2).
struct DeviceCapabilities {
    Backend backend = Backend::Null;
    bool multiDrawIndirectCount = false; // glMultiDrawElementsIndirectCount (GL 4.6).
    bool bindlessTextures = false;
    bool spirvShaders = false;
    bool timestampQueries = false;
    u32 maxTextureArrayLayers = 0;
    u32 maxTextureSize = 0;
    u32 maxComputeWorkGroupInvocations = 0;
    f32 maxAnisotropy = 1.0F;
    std::string rendererName; // For example "NVIDIA GeForce RTX 3060".
    std::string vendorName;
    std::string driverVersion;
};

// Memory and object counts, for the GPU page of the overlay and leak reports.
struct DeviceStats {
    u64 bufferBytes = 0;
    u64 textureBytes = 0;
    u32 buffers = 0;
    u32 textures = 0;
    u32 samplers = 0;
    u32 shaders = 0;
    u32 pipelines = 0;
    u64 commandsSubmitted = 0; // Since startup.
    u64 framesPresented = 0;
};

// The device is the only virtual interface in the RHI. Everything here is
// called at low frequency (resource creation, uploads, submission,
// presentation), so the cost of a virtual call does not matter. Per command
// work happens in CommandList, which is not virtual.
//
// Thread safety: main thread only (Section 6.4). Worker threads may write
// into memory returned by mappedRange(), but never call the device.
class Device {
public:
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;
    virtual ~Device() = default;

    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual DeviceStats stats() const = 0;

    [[nodiscard]] virtual Result<BufferHandle> createBuffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual Result<TextureHandle> createTexture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual Result<SamplerHandle> createSampler(const SamplerDesc& desc) = 0;
    [[nodiscard]] virtual Result<ShaderHandle> createShader(const ShaderDesc& desc) = 0;
    [[nodiscard]] virtual Result<PipelineHandle> createPipeline(const PipelineDesc& desc) = 0;
    virtual void destroy(BufferHandle handle) = 0;
    virtual void destroy(TextureHandle handle) = 0;
    virtual void destroy(SamplerHandle handle) = 0;
    virtual void destroy(ShaderHandle handle) = 0;
    virtual void destroy(PipelineHandle handle) = 0;

    // Copies data into a buffer right away. For small, rare updates; bulk
    // per frame uploads go through mapped staging memory (Section 7.5).
    [[nodiscard]] virtual Result<void> updateBuffer(BufferHandle buffer, usize offset,
                                                    std::span<const std::byte> data) = 0;

    // Uploads one mip level of one layer of a texture.
    [[nodiscard]] virtual Result<void> uploadTexture(TextureHandle texture, const TextureUpload& upload) = 0;

    // Returns a CPU pointer to a buffer created with BufferUsage::CpuWrite.
    // The pointer stays valid for the lifetime of the buffer. Worker threads
    // may write into it, but the caller must use fences so the GPU is not
    // reading the same bytes at the same time. Empty for other buffers.
    [[nodiscard]] virtual std::span<std::byte> mappedRange(BufferHandle buffer) = 0;

    // Fences mark a point in the GPU's command stream; isFenceSignaled is
    // true once the GPU has passed it. Signaled fences should be destroyed.
    [[nodiscard]] virtual FenceHandle insertFence() = 0;
    [[nodiscard]] virtual bool isFenceSignaled(FenceHandle fence) = 0;
    virtual void destroy(FenceHandle fence) = 0;

    // Executes a recorded command list. The OpenGL backend replays it at once.
    virtual void submit(const CommandList& commands) = 0;

    virtual void beginFrame() = 0;
    virtual void present() = 0;
    virtual void setPresentMode(PresentMode mode) = 0;
    // The size of the window's back buffer in pixels. 0 for headless devices.
    [[nodiscard]] virtual u32 backBufferWidth() const noexcept = 0;
    [[nodiscard]] virtual u32 backBufferHeight() const noexcept = 0;
};

} // namespace vxe::rhi
