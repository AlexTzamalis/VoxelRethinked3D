#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <imgui.h>

#include <array>
#include <memory>

namespace vxe::debug {

// Draws Dear ImGui through the RHI (E7.2), instead of ImGui's own OpenGL
// backend, so developer tools work on every RHI backend and never touch GL
// directly (ADR-003, ADR-013).
//
// Vertices are pulled from a storage buffer by the vertex shader
// (engine/shaders/debug/imgui.vert), indices come from an index buffer. Both
// live in persistently mapped buffers, three sets in a ring: the CPU writes
// frame N+1 while the GPU may still read frame N. A fence guards each set
// before it is written again.
//
// Textures follow ImGui's protocol (ImGui 1.92 and newer): ImGui asks the
// renderer to create, update and destroy textures, for example when the font
// atlas grows because new glyphs were used.
//
// Thread safety: main thread only, with the right ImGui context current.
class ImGuiRenderer {
public:
    static constexpr usize kFramesInFlight = 3;

    // Creates the pipeline and sampler. `shaders` reads the engine shader
    // folder; `targetFormat` is the color format of the pass ImGui draws into.
    [[nodiscard]] static Result<std::unique_ptr<ImGuiRenderer>>
    create(rhi::Device& device, const rhi::ShaderFileReader& shaders, rhi::Format targetFormat);
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    ImGuiRenderer(ImGuiRenderer&&) = delete;
    ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;

    // Creates, updates and destroys the textures ImGui asked for in this
    // frame. Call after ImGui::Render(), before record().
    void updateTextures(ImDrawData& drawData);

    // Records the draw commands of `drawData` into the caller's open render
    // pass, which must cover the framebuffer size ImGui was given.
    void record(rhi::CommandList& commands, const ImDrawData& drawData);

    // Destroys every texture ImGui still holds. Call before destroying the
    // ImGui context, while it is current.
    void releaseTextures();

    // The ImTextureID for showing an engine texture with ImGui::Image. The
    // texture needs TextureUsage::Sampled.
    [[nodiscard]] static ImTextureID textureId(rhi::TextureHandle texture) noexcept;
    [[nodiscard]] static rhi::TextureHandle textureHandle(ImTextureID id) noexcept;

private:
    explicit ImGuiRenderer(rhi::Device& device)
        : m_device(device) {}

    // One frame's worth of buffers in the ring.
    struct FrameBuffers {
        rhi::BufferHandle vertices;
        usize vertexCapacity = 0; // Bytes.
        rhi::BufferHandle indices;
        usize indexCapacity = 0;
        rhi::BufferHandle uniforms;
        rhi::FenceHandle fence; // Signals when the GPU finished the frame that used this set.
    };

    void createTexture(ImTextureData& texture);
    void uploadTexture(ImTextureData& texture);
    void destroyTexture(ImTextureData& texture);
    Result<void> ensureCapacity(rhi::BufferHandle& buffer, usize& capacity, usize needed,
                                rhi::BufferUsage usage, std::string_view name);
    void waitForSet(FrameBuffers& set);
    void bindState(rhi::CommandList& commands, const FrameBuffers& set, usize vertexBytes) const;

    rhi::Device& m_device;
    rhi::PipelineHandle m_pipeline;
    rhi::SamplerHandle m_sampler;
    std::array<FrameBuffers, kFramesInFlight> m_frames{};
    usize m_frame = 0;
    bool m_recordedOnce = false;
};

} // namespace vxe::debug
