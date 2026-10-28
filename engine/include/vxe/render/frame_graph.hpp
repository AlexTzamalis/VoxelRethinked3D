#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/gpu_timer.hpp>
#include <vxe/rhi/types.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::render {

// A texture known to the frame graph: one it creates (transient) or one
// created elsewhere and imported. Only valid for the graph that made it.
struct TextureRef {
    static constexpr u32 kInvalid = 0xFFFFFFFF;
    u32 index = kInvalid;
    [[nodiscard]] constexpr bool isValid() const noexcept { return index != kInvalid; }
    friend constexpr bool operator==(TextureRef, TextureRef) = default;
};

// A buffer imported into the frame graph, so passes can declare how they use
// it and the graph can place barriers between them.
struct BufferRef {
    static constexpr u32 kInvalid = 0xFFFFFFFF;
    u32 index = kInvalid;
    [[nodiscard]] constexpr bool isValid() const noexcept { return index != kInvalid; }
    friend constexpr bool operator==(BufferRef, BufferRef) = default;
};

enum class PassType : u8 {
    Raster,  // Draws into attachments; the graph begins and ends the render pass around it.
    Compute, // Dispatches; no render pass.
    Transfer // Copies and uploads; no render pass.
};

// A texture the graph creates for the frame (render targets, depth buffers,
// intermediate images). Its size follows the graph's output size unless a
// fixed size is given, so window resizes need no code in the passes.
struct TransientTextureDesc {
    rhi::Format format = rhi::Format::RGBA8Unorm;
    f32 scale = 1.0F; // Of the output size, for example 0.5 for a half resolution bloom target.
    u32 width = 0;    // A fixed size when both are set; scale is then ignored.
    u32 height = 0;
    // Usage beyond what the declared accesses imply (they add RenderTarget,
    // DepthTarget, Sampled and Storage themselves), for example TransferSrc
    // so tests and screenshots can read it back.
    rhi::TextureUsage extraUsage = rhi::TextureUsage::None;
};

// How a pass reads or writes an imported buffer. The graph turns these into
// barriers after shader storage writes (Section 7.4: resourceBarrier).
enum class BufferAccess : u8 {
    Vertex,
    Index,
    Uniform,
    Indirect,
    StorageRead,
    StorageWrite, // Shader writes (compute culling, Hi-Z). Later accesses get a barrier.
    TransferSrc,
    TransferDst,
};

class FrameGraph;

// What a pass's record function gets: the command list and the real GPU
// objects behind the graph's references for this frame.
struct PassContext {
    rhi::Device& device;
    rhi::CommandList& commands;
    const FrameGraph& graph;
    u32 width = 0; // The graph's output size.
    u32 height = 0;

    [[nodiscard]] rhi::TextureHandle texture(TextureRef ref) const;
    [[nodiscard]] rhi::BufferHandle buffer(BufferRef ref) const;
};

using PassRecordFunction = std::function<void(PassContext&)>;

struct PassDesc {
    // Letters and digits only. Names the debug group, the toggle setting
    // Debug.Passes.<name> and the stat Gpu.Pass.<name>.
    std::string name{};
    PassType type = PassType::Raster;
    // false for passes the program cannot work without (the window pass that
    // draws the debug UI, for example). They get no toggle setting.
    bool canDisable = true;
    PassRecordFunction record{};
};

// Declares what one pass reads and writes. Returned by FrameGraph::addPass
// and only used while setting the graph up (before compile).
class PassBuilder {
public:
    // A new transient texture, first written by this pass.
    [[nodiscard]] TextureRef createTexture(std::string_view name, const TransientTextureDesc& desc);

    // Raster attachments, in the order of the pass's color outputs. Clear
    // and DontCare mean the old contents are not needed; Load keeps them.
    void colorAttachment(TextureRef texture, rhi::LoadOp load = rhi::LoadOp::Clear,
                         std::array<f32, 4> clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
                         rhi::StoreOp store = rhi::StoreOp::Store);
    void depthAttachment(TextureRef texture, rhi::LoadOp load = rhi::LoadOp::Clear, f32 clearDepth = 0.0F,
                         rhi::StoreOp store = rhi::StoreOp::Store);
    // The window's back buffer. Cannot be combined with texture attachments
    // in one pass (OpenGL keeps the window's framebuffer separate).
    void windowAttachment(rhi::LoadOp load = rhi::LoadOp::Clear,
                          std::array<f32, 4> clearColor = {0.0F, 0.0F, 0.0F, 1.0F});

    // The pass samples the texture in shaders.
    void sampleTexture(TextureRef texture);
    // The pass reads or writes the texture as a storage image (compute).
    void readStorageTexture(TextureRef texture);
    void writeStorageTexture(TextureRef texture);
    void useBuffer(BufferRef buffer, BufferAccess access);

private:
    friend class FrameGraph;
    PassBuilder(FrameGraph& graph, u32 pass)
        : m_graph(&graph)
        , m_pass(pass) {}
    FrameGraph* m_graph;
    u32 m_pass;
};

struct FrameGraphDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr; // Optional: no toggles without it.
    stats::StatRegistry* stats = nullptr;         // Optional: no stats without it.
};

// The "frame graph lite" of Section 8.1: the renderer's passes in the order
// they run, with the textures and buffers each one reads and writes.
//
// From those declarations the graph
// - creates the transient textures at the right size and recreates them when
//   the output size changes (setOutputSize),
// - lets transient textures with the same format, size and usage share one
//   GPU texture when their lifetimes do not overlap (Debug.FrameGraph.Aliasing),
// - inserts a barrier before a pass that uses what an earlier pass wrote with
//   shader storage writes, the only writes OpenGL does not order by itself,
// - gives every pass a live toggle (Debug.Passes.<name>): a disabled pass
//   still clears the attachments it would have cleared, so later passes never
//   see stale or undefined contents,
// - measures every pass on the GPU (stat Gpu.Pass.<name>, a few frames late,
//   see rhi::GpuFrameTimer) and wraps it in a debug group for RenderDoc.
//
// It is "lite" because the pass list is fixed after compile and runs in the
// order it was added: no automatic reordering and no culling of unused passes.
// Recording a frame does not allocate once the command list has grown.
//
// Usage: addPass and declare resources, compile() once, then every frame
// setOutputSize() and execute().
//
// Thread safety: main thread only, like the device it records for.
class FrameGraph {
public:
    explicit FrameGraph(const FrameGraphDesc& desc);
    ~FrameGraph();
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;
    FrameGraph(FrameGraph&&) = delete;
    FrameGraph& operator=(FrameGraph&&) = delete;

    // Adds a pass after the ones added before. Only before compile().
    [[nodiscard]] PassBuilder addPass(PassDesc desc);
    // Makes a texture or buffer created elsewhere usable by passes. The
    // handle may be swapped every frame (ping pong targets).
    [[nodiscard]] TextureRef importTexture(std::string_view name, rhi::TextureHandle texture);
    [[nodiscard]] BufferRef importBuffer(std::string_view name, rhi::BufferHandle buffer);
    void setImportedTexture(TextureRef ref, rhi::TextureHandle texture);
    void setImportedBuffer(BufferRef ref, rhi::BufferHandle buffer);

    // Checks the declarations, plans barriers and registers settings, stats
    // and GPU timers. Fails with a message naming the pass and resource for:
    // reading a texture no earlier pass wrote, sampling a texture the same
    // pass renders into, loading a transient texture before anything wrote it,
    // raster passes without attachments, window plus texture attachments in
    // one pass, attachments on compute or transfer passes, and bad names.
    [[nodiscard]] Result<void> compile();
    [[nodiscard]] bool isCompiled() const noexcept { return m_compiled; }

    // The size transient textures are measured against, usually the window's.
    void setOutputSize(u32 width, u32 height) noexcept;

    // Records every pass into `commands`, creating or recreating transient
    // textures first when the size or the aliasing setting changed.
    [[nodiscard]] Result<void> execute(rhi::CommandList& commands);

    // Inspection, for tests and the overlay.
    [[nodiscard]] u32 passCount() const noexcept { return static_cast<u32>(m_passes.size()); }
    [[nodiscard]] const std::string& passName(u32 pass) const { return m_passes.at(pass).desc.name; }
    [[nodiscard]] bool isPassEnabled(u32 pass) const;
    // GPU time of the pass from a few frames ago, if measured.
    [[nodiscard]] std::optional<f64> passGpuMilliseconds(u32 pass) const;
    // GPU textures backing the transient textures, and their total size.
    [[nodiscard]] u32 physicalTextureCount() const noexcept { return static_cast<u32>(m_physical.size()); }
    [[nodiscard]] u64 transientBytes() const noexcept { return m_transientBytes; }
    // Barriers recorded per frame.
    [[nodiscard]] u32 barrierCount() const noexcept { return m_barrierCount; }
    [[nodiscard]] rhi::TextureHandle texture(TextureRef ref) const;
    [[nodiscard]] rhi::BufferHandle buffer(BufferRef ref) const;

private:
    friend class PassBuilder;

    enum class TextureAccess : u8 {
        ColorAttachment,
        DepthAttachment,
        Sampled,
        StorageRead,
        StorageWrite
    };
    struct TextureUse {
        TextureRef texture{};
        TextureAccess access = TextureAccess::Sampled;
        rhi::LoadOp load = rhi::LoadOp::Load; // Attachments only.
        rhi::StoreOp store = rhi::StoreOp::Store;
        std::array<f32, 4> clearColor{};
        f32 clearDepth = 0.0F;
    };
    struct BufferUse {
        BufferRef buffer{};
        BufferAccess access = BufferAccess::Uniform;
    };
    struct Pass {
        PassDesc desc{};
        std::vector<TextureUse> textures{};
        std::vector<BufferUse> buffers{};
        bool window = false;
        rhi::LoadOp windowLoad = rhi::LoadOp::Clear;
        std::array<f32, 4> windowClear{};
        // Planned by compile:
        rhi::BarrierScope barrier = rhi::BarrierScope::None;
        bool clearsWhenDisabled = false;
        config::Setting<bool> enabled{};
        bool hasToggle = false;
        stats::StatId gpuTime{};
        std::unique_ptr<rhi::GpuFrameTimer> timer{};
    };
    struct Texture {
        std::string name{};
        bool imported = false;
        rhi::TextureHandle handle{}; // Imported textures only; transients use m_physical.
        TransientTextureDesc desc{};
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        std::optional<u32> firstPass{}; // Lifetime, from compile.
        u32 lastPass = 0;
        u32 physical = 0; // Index into m_physical, set when allocated.
    };
    struct Buffer {
        std::string name{};
        rhi::BufferHandle handle{};
    };
    struct Physical {
        rhi::TextureHandle handle{};
        rhi::Format format = rhi::Format::Undefined;
        u32 width = 0;
        u32 height = 0;
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        u32 lastPass = 0;
        std::string debugName{};
    };

    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] Result<void> validateShape(usize index) const;
    [[nodiscard]] Result<void> validateTextureUses(const Pass& pass, std::vector<bool>& written) const;
    void planBarriers();
    void planDisabledClears();
    void registerControls();
    [[nodiscard]] Result<void> allocate();
    void releaseTextures();
    [[nodiscard]] u32 textureWidth(const Texture& texture) const noexcept;
    [[nodiscard]] u32 textureHeight(const Texture& texture) const noexcept;
    [[nodiscard]] rhi::RenderPassDesc renderPass(const Pass& pass, bool onlyClears) const;
    [[nodiscard]] static Error error(const Pass& pass, std::string_view message);

    rhi::Device& m_device;
    config::SettingsRegistry* m_settings;
    stats::StatRegistry* m_stats;
    config::Setting<bool> m_aliasing;
    stats::StatId m_transientBytesStat;
    stats::StatId m_barriersStat;
    stats::StatId m_passesStat;

    std::vector<Pass> m_passes;
    std::vector<Texture> m_textures;
    std::vector<Buffer> m_buffers;
    std::vector<Physical> m_physical;
    bool m_compiled = false;
    bool m_allocated = false;
    bool m_allocatedWithAliasing = false;
    u32 m_width = 1;
    u32 m_height = 1;
    u32 m_allocatedWidth = 0;
    u32 m_allocatedHeight = 0;
    u64 m_transientBytes = 0;
    u32 m_barrierCount = 0;
};

} // namespace vxe::render
