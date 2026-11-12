#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/render/clouds.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/entity_renderer.hpp>
#include <vxe/render/frame_constants.hpp>
#include <vxe/render/frame_graph.hpp>
#include <vxe/render/god_rays.hpp>
#include <vxe/render/lod_renderer.hpp>
#include <vxe/render/post_process.hpp>
#include <vxe/render/precipitation.hpp>
#include <vxe/render/screenshot.hpp>
#include <vxe/render/sky.hpp>
#include <vxe/render/sunlight.hpp>
#include <vxe/render/water.hpp>
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
    usize chunkArenaBytes = usize{1024} << 20U;
    u32 maxChunks = 131072;
    usize lodArenaBytes = usize{512} << 20U;
};

// The engine's standard frame (Section 8.1), owned by the main loop: every
// renderer piece built in E8 and E9 joined in one frame graph.
//
//   ChunkUpload        new chunk meshes into the vertex arena (transfer)
//   Visibility         chunk culling: frustum, face direction, caves, occlusion (compute)
//   LongShadows        how high mountain shade reaches over each column (compute, when needed)
//   Shadows            the sun's or moon's shadow map of nearby terrain
//   LodUpload          new far terrain tiles into their arena (transfer)
//   Chunks             opaque and cutout chunks into the HDR scene
//   Lod                far terrain, hidden over columns real chunks draw
//   Sky                sky where nothing was drawn, height fog over the rest
//   ChunksTranslucent  water and glass, blended
//   FirstPerson        the hand and the item in it, over the world
//   HiZ                depth pyramid for next frame's occlusion culling (compute)
//   DebugDraw          debug lines (chunk borders, boxes, grids, a frozen culling frustum)
//   Post               tonemapping into the window
//   Ui                 the game UI the owner hooks in (setUi, Section 19)
//   WindowOverlay      whatever the owner hooks in (the debug UI), never disabled
//
// Each pass can be switched off with Debug.Passes.<name> and is timed on the
// GPU (Gpu.Pass.<name>).
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
    // Recorded into the window after post processing, before the window
    // overlay: the game UI (ui::UiRenderer lives above render, ADR-015).
    void setUi(std::function<void(rhi::CommandList&)> record);
    // Reads the next frame back from the window once it is drawn (UI
    // included) and hands it over on the main thread, inside endFrame. Slow
    // for that one frame: it waits for the GPU.
    void requestScreenshot(std::function<void(Screenshot)> done);
    // The engine shader reader, for renderers built above this layer.
    [[nodiscard]] const rhi::ShaderFileReader& shaders() const noexcept { return m_shaders; }

    [[nodiscard]] const Camera& camera() const noexcept { return m_camera; }
    [[nodiscard]] ChunkRenderer& chunks() noexcept { return *m_chunks; }
    [[nodiscard]] LodRenderer& lod() noexcept { return *m_lod; }
    [[nodiscard]] DebugDraw& debugDraw() noexcept { return *m_debugDraw; }
    [[nodiscard]] SkyRenderer& sky() noexcept { return *m_sky; }
    [[nodiscard]] CloudRenderer& clouds() noexcept { return *m_clouds; }
    [[nodiscard]] PrecipitationRenderer& precipitation() noexcept { return *m_precipitation; }
    [[nodiscard]] SunlightShadows& sunlight() noexcept { return *m_sunlight; }
    [[nodiscard]] EntityRenderer& entities() noexcept { return *m_entities; }
    [[nodiscard]] WaterRenderer& water() noexcept { return *m_water; }
    // How strong sky light is right now, 0.25 (moonlight) to 1 (day), as the
    // chunks use it: for lighting entities the same way.
    [[nodiscard]] f32 daylight() const noexcept;
    [[nodiscard]] rhi::Device& device() noexcept { return m_device; }
    // The GPU time of a recent frame (a few frames old, measured without stalls).
    [[nodiscard]] std::optional<f64> gpuMilliseconds() const noexcept {
        return m_timer.lastFrameMilliseconds();
    }

private:
    explicit Renderer(rhi::Device& device) noexcept
        : m_device(device) {}
    [[nodiscard]] Result<void> buildGraph();
    void takeScreenshots();

    rhi::Device& m_device;
    Camera m_camera;
    std::unique_ptr<FrameGraph> m_graph;
    FrameConstantsBuffer m_constants;
    std::unique_ptr<ChunkRenderer> m_chunks;
    std::unique_ptr<LodRenderer> m_lod;
    std::unique_ptr<SkyRenderer> m_sky;
    std::unique_ptr<CloudRenderer> m_clouds;
    std::unique_ptr<GodRayRenderer> m_godRays;
    std::unique_ptr<WaterRenderer> m_water;
    std::unique_ptr<SunlightShadows> m_sunlight;
    std::unique_ptr<EntityRenderer> m_entities;
    std::unique_ptr<PrecipitationRenderer> m_precipitation;
    std::unique_ptr<PostProcess> m_post;
    std::unique_ptr<DebugDraw> m_debugDraw;
    rhi::GpuFrameTimer m_timer;
    bool m_timerCreated = false;
    rhi::CommandList m_commands;
    std::function<void(rhi::CommandList&)> m_windowOverlay;
    std::function<void(rhi::CommandList&)> m_ui;
    std::vector<std::function<void(Screenshot)>> m_screenshots;
    rhi::ShaderFileReader m_shaders;
    bool m_frameBegun = false;
    u64 m_frameIndex = 0;
};

} // namespace vxe::render
