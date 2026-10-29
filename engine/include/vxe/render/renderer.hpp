#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/frame_constants.hpp>
#include <vxe/render/frame_graph.hpp>
#include <vxe/render/post_process.hpp>
#include <vxe/render/sky.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/gpu_timer.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <functional>
#include <memory>
#include <optional>

namespace vxe::render {

struct RendererDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    input::InputSystem* input = nullptr;  // Optional: debug draw keys (F3+G).
    input::InputContextId debugContext{}; // The always active context for debug keys.
    rhi::ShaderFileReader shaders{};
    usize chunkArenaBytes = usize{256} << 20U;
    u32 maxChunks = 32768;
};

// The engine's standard frame (Section 8.1), owned by the main loop: every
// renderer piece built in E8 and E9 joined in one frame graph.
//
//   ChunkUpload        new chunk meshes into the vertex arena (transfer)
//   Visibility         chunk culling: frustum, face direction, caves, occlusion (compute)
//   Chunks             opaque and cutout chunks into the HDR scene
//   Sky                sky where nothing was drawn, height fog over the rest
//   ChunksTranslucent  water and glass, blended
//   HiZ                depth pyramid for next frame's occlusion culling (compute)
//   DebugDraw          debug lines (chunk borders, boxes, grids, a frozen culling frustum)
//   Post               tonemapping into the window
//   WindowOverlay      whatever the owner hooks in (the debug UI), never disabled
//
// Each pass can be switched off with Debug.Passes.<name> and is timed on the
// GPU (Gpu.Pass.<name>). Passes not built yet (LOD, game UI) slot in here in
// their steps.
//
// A frame: the application sets the camera and calls beginFrame once its
// camera is final for the frame, then uploads chunks and adds debug lines;
// the main loop calls endFrame, which records, submits and presents. If the
// application never calls beginFrame, endFrame does it with the last camera.
//
// Thread safety: main thread only, like the device.
class Renderer {
public:
    [[nodiscard]] static Result<std::unique_ptr<Renderer>> create(const RendererDesc& desc);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Starts the frame from `camera`: the sky's clock, frame constants, the
    // chunk renderer's staging memory and the debug lines' origin.
    void beginFrame(const Camera& camera, const FrameTime& time);
    [[nodiscard]] bool frameBegun() const noexcept { return m_frameBegun; }
    // Records and submits the frame and presents it.
    [[nodiscard]] Result<void> endFrame(const FrameTime& time);

    // Recorded into the window after post processing: the debug UI.
    void setWindowOverlay(std::function<void(rhi::CommandList&)> record);

    [[nodiscard]] const Camera& camera() const noexcept { return m_camera; }
    [[nodiscard]] ChunkRenderer& chunks() noexcept { return *m_chunks; }
    [[nodiscard]] DebugDraw& debugDraw() noexcept { return *m_debugDraw; }
    [[nodiscard]] SkyRenderer& sky() noexcept { return *m_sky; }
    [[nodiscard]] rhi::Device& device() noexcept { return m_device; }
    // The GPU time of a recent frame (a few frames old, measured without stalls).
    [[nodiscard]] std::optional<f64> gpuMilliseconds() const noexcept {
        return m_timer.lastFrameMilliseconds();
    }

private:
    explicit Renderer(rhi::Device& device) noexcept
        : m_device(device) {}
    [[nodiscard]] Result<void> buildGraph();

    rhi::Device& m_device;
    Camera m_camera;
    std::unique_ptr<FrameGraph> m_graph;
    FrameConstantsBuffer m_constants;
    std::unique_ptr<ChunkRenderer> m_chunks;
    std::unique_ptr<SkyRenderer> m_sky;
    std::unique_ptr<PostProcess> m_post;
    std::unique_ptr<DebugDraw> m_debugDraw;
    rhi::GpuFrameTimer m_timer;
    bool m_timerCreated = false;
    rhi::CommandList m_commands;
    std::function<void(rhi::CommandList&)> m_windowOverlay;
    bool m_frameBegun = false;
    u64 m_frameIndex = 0;
};

} // namespace vxe::render
