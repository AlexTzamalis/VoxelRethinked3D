#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/handles.hpp>
#include <vxe/rhi/types.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

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
    // Offsets passed to bindUniformBuffer and bindStorageBuffer must be
    // multiples of these (often 256 and 16 or 32 bytes).
    u32 uniformBufferOffsetAlignment = 256;
    u32 storageBufferOffsetAlignment = 256;
    f32 maxAnisotropy = 1.0F;
    bool debugOutput = false; // API messages (KHR_debug) are reported to the log and counted.
    u64 videoMemoryBytes = 0; // Dedicated video memory; 0 when the driver does not say.
    i32 apiVersionMajor = 0;  // For example 4 and 6 for OpenGL 4.6.
    i32 apiVersionMinor = 0;
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
    u64 commandsSubmitted = 0;    // Since startup.
    u64 invalidCommands = 0;      // Commands that used destroyed resources or broke nesting rules.
    u64 apiErrors = 0;            // Errors the graphics API reported (KHR_debug). Should stay 0.
    u64 apiWarnings = 0;          // Performance and portability warnings it reported.
    u64 pipelineBinds = 0;        // Pipeline binds submitted.
    u64 pipelineBindsSkipped = 0; // Binds of the pipeline that was already bound, filtered out.
    u64 stateChanges = 0;         // Fixed function state calls actually sent to the API.
    u64 drawCalls = 0;            // Draws sent to the API, each indirect multi draw counting once.
    u64 dispatches = 0;           // Compute dispatches sent to the API.
    u32 pendingDestroys = 0;      // Destroyed objects waiting for the GPU to finish with them.
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

    // Copies buffer contents back to the CPU. Slow: it waits for the GPU to
    // finish with the buffer. Meant for tests and debug views (for example
    // reading culling counts into the overlay), never for per frame paths.
    [[nodiscard]] virtual Result<void> readBuffer(BufferHandle buffer, usize offset,
                                                  std::span<std::byte> out) = 0;

    // Copies one mip level of one layer of a texture back to the CPU, tightly
    // packed and bottom row first like TextureUpload. Slow, like readBuffer: for tests, screenshots
    // and debug views only. Needs TextureUsage::TransferSrc.
    [[nodiscard]] virtual Result<void> readTexture(TextureHandle texture, u32 mipLevel, u32 layer,
                                                   std::span<std::byte> out) = 0;

    // Uploads one mip level of one layer of a texture.
    [[nodiscard]] virtual Result<void> uploadTexture(TextureHandle texture, const TextureUpload& upload) = 0;

    // Returns a CPU pointer to a buffer created with BufferUsage::CpuWrite or
    // CpuRead (for CpuRead: read only after a fence covering the GPU write).
    // The pointer stays valid for the lifetime of the buffer. Worker threads
    // may write into it, but the caller must use fences so the GPU is not
    // reading the same bytes at the same time. Empty for other buffers.
    [[nodiscard]] virtual std::span<std::byte> mappedRange(BufferHandle buffer) = 0;

    // Fences mark a point in the GPU's command stream; isFenceSignaled is
    // true once the GPU has passed it. Signaled fences should be destroyed.
    [[nodiscard]] virtual FenceHandle insertFence() = 0;
    [[nodiscard]] virtual bool isFenceSignaled(FenceHandle fence) = 0;
    virtual void destroy(FenceHandle fence) = 0;

    // Timestamp queries measure GPU time. CommandList::writeTimestamp records
    // the GPU clock when the GPU reaches that point; the result arrives a few
    // frames later. timestampNanoseconds never waits: it returns nothing until
    // the GPU has written the newest timestamp of that query. GpuFrameTimer
    // (gpu_timer.hpp) wraps this into a stall free frame time.
    [[nodiscard]] virtual Result<QueryHandle> createTimestampQuery() = 0;
    virtual void destroy(QueryHandle query) = 0;
    [[nodiscard]] virtual std::optional<u64> timestampNanoseconds(QueryHandle query) = 0;

    // Executes a recorded command list. The OpenGL backend replays it at once.
    virtual void submit(const CommandList& commands) = 0;

    // Frames: beginFrame() starts one, present() ends it and shows it. The
    // device counts frames to know when destroyed objects are no longer in
    // use by the GPU (see DeferredReleaseQueue) and frees them in beginFrame.
    virtual void beginFrame() = 0;
    virtual void present() = 0;

    // Blocks until the GPU has finished all submitted work, then frees every
    // object whose destruction was deferred. For shutdown, window resizes and
    // tests; never call it every frame, it drains the GPU pipeline.
    virtual void waitIdle() = 0;

    // Every object created through this device and not destroyed yet, as
    // "buffer 'Chunk arena'". Empty at a clean shutdown. The device logs this
    // list as a leak report when it is destroyed (E6.6).
    [[nodiscard]] virtual std::vector<std::string> liveResources() const = 0;

    virtual void setPresentMode(PresentMode mode) = 0;
    // The size of the window's back buffer in pixels. 0 for headless devices.
    [[nodiscard]] virtual u32 backBufferWidth() const noexcept = 0;
    [[nodiscard]] virtual u32 backBufferHeight() const noexcept = 0;
};

} // namespace vxe::rhi
