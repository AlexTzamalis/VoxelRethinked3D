#pragma once

// Command list replay for the OpenGL backend (Section 7.4, E6.5).

#include "gl_resources.hpp"
#include "gl_state.hpp"

#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>

#include <glad/gl.h>

#include <array>
#include <optional>
#include <vector>

namespace vxe::platform {
class Window;
}

namespace vxe::rhi::gl {

struct ReplayStats {
    u64 commands = 0;
    u64 invalidCommands = 0;
    u64 pipelineBinds = 0;
    u64 pipelineBindsSkipped = 0;
    u64 drawCalls = 0;
    u64 dispatches = 0;
};

// Turns recorded commands into OpenGL calls.
//
// Every command is checked before anything reaches OpenGL: handles must be
// alive, usage flags must match (a uniform bind needs a Uniform buffer),
// ranges must fit, and commands must appear where they are allowed (draws
// inside render passes, dispatches and copies outside). An invalid command
// is counted and skipped instead of being sent, so a renderer bug shows up
// in DeviceStats::invalidCommands instead of as driver errors or a crash.
//
// Thread safety: main thread only, like the device that owns it.
class CommandReplayer {
public:
    CommandReplayer(Resources& resources, StateCache& state, const DeviceCapabilities& capabilities,
                    platform::Window& window)
        : m_resources(resources)
        , m_state(state)
        , m_capabilities(capabilities)
        , m_window(window) {}

    // Creates the engine's single vertex array object and reads the binding
    // limits. Needs the GL context.
    void initialize();
    // Deletes the vertex array and every cached framebuffer.
    void shutdown();

    void submit(const CommandList& commands);

    // The device calls these before deleting objects the replayer remembers.
    void onPipelineDestroyed(PipelineHandle handle) noexcept;
    void onTextureDestroyed(GLuint texture);

    [[nodiscard]] const ReplayStats& stats() const noexcept { return m_stats; }

private:
    // Each returns false if the command is invalid, and then changes nothing.
    bool replay(const CommandView& command);
    bool beginRenderPass(const RenderPassDesc& pass);
    bool endRenderPass();
    bool bindPipeline(PipelineHandle handle);
    bool bindBufferRange(GLenum target, BufferUsage requiredUsage, u32 alignment, u32 maxBindings,
                         const cmd::BindBufferRange& bind);
    bool bindTexture(const cmd::BindTexture& bind);
    bool bindIndexBuffer(const cmd::BindIndexBuffer& bind);
    [[nodiscard]] bool setViewport(const cmd::SetViewport& viewport) const;
    bool setScissor(const cmd::SetScissor& scissor);
    bool draw(const cmd::Draw& draw);
    bool drawIndexed(const cmd::DrawIndexed& draw);
    bool multiDrawIndirect(const cmd::MultiDrawIndexedIndirect& draw);
    bool multiDrawIndirectCount(const cmd::MultiDrawIndexedIndirectCount& draw);
    bool dispatch(const cmd::Dispatch& dispatch);
    bool copyBuffer(const cmd::CopyBuffer& copy);
    static bool barrier(const cmd::Barrier& barrier);
    bool beginDebugGroup(const CommandView& command);
    bool endDebugGroup();
    bool writeTimestamp(const cmd::WriteTimestamp& timestamp);

    // Checks shared by the draw commands.
    [[nodiscard]] bool canDraw() const noexcept;
    [[nodiscard]] bool isIndirectRangeValid(BufferHandle handle, usize offset, u32 drawCount,
                                            u32 stride) const;
    static void bindIndirectBuffer(GLenum target, GLuint buffer, std::optional<GLuint>& bound);

    // The framebuffer for a pass: 0 for the window, a cached framebuffer
    // object for textures, nothing if the attachments are unusable.
    [[nodiscard]] std::optional<GLuint> framebufferFor(const RenderPassDesc& pass);
    void clearAttachments(GLuint framebuffer, const RenderPassDesc& pass);
    void invalidateAttachments(GLuint framebuffer, const RenderPassDesc& pass, bool atEnd);
    void setScissorEnabled(bool enabled);

    // Attachments that make up one framebuffer object, for the cache.
    struct FramebufferKey {
        std::array<GLuint, kMaxColorAttachments> colors{};
        u32 colorCount = 0;
        GLuint depth = 0;
        bool depthStencil = false;
        friend bool operator==(const FramebufferKey&, const FramebufferKey&) = default;
    };
    struct CachedFramebuffer {
        FramebufferKey key;
        GLuint framebuffer = 0;
    };

    // The index buffer bound for indexed draws.
    struct IndexBinding {
        GLenum type = GL_UNSIGNED_INT;
        u32 indexSize = 4;
        usize offset = 0;
    };

    // OpenGL limits that decide which binds are valid.
    struct Limits {
        u32 uniformBufferBindings = 0;
        u32 storageBufferBindings = 0;
        u32 textureUnits = 0;
        std::array<u32, 3> computeGroups{};
    };

    Resources& m_resources;
    StateCache& m_state;
    const DeviceCapabilities& m_capabilities;
    platform::Window& m_window;

    GLuint m_vertexArray = 0;
    Limits m_limits;
    std::vector<CachedFramebuffer> m_framebuffers;

    // Replay state. It persists between submits, like GL state does.
    bool m_insidePass = false;
    RenderPassDesc m_pass{};
    GLuint m_framebuffer = 0;
    PipelineHandle m_boundPipeline;
    bool m_pipelineIsCompute = false;
    GLenum m_topology = GL_TRIANGLES;
    std::optional<IndexBinding> m_indexBuffer;
    std::optional<GLuint> m_drawIndirectBuffer;
    std::optional<GLuint> m_parameterBuffer;
    std::optional<bool> m_scissorEnabled;
    std::optional<bool> m_srgbWrites;
    u32 m_debugGroupDepth = 0;
    ReplayStats m_stats;
};

} // namespace vxe::rhi::gl
