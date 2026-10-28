// vxe_rhi_demo: a spinning cube and a colored triangle drawn through the RHI
// (Section 7, the E6.5 acceptance demo).
//
// Usage: vxe_rhi_demo [--frames N] [--vsync] [--no-ui] [--imgui-demo] [--overlay-page N]
//
// The cube is drawn into an offscreen color and depth target with reversed Z,
// then copied to the window with a fullscreen triangle, and the triangle is
// drawn on top. The window title shows the GPU frame time (measured with
// timestamp queries, without stalls), the CPU frame time and the frame rate.
// With --frames the demo stops after N frames and prints a summary; the exit
// code is non zero if the device reported invalid commands or driver errors,
// so the demo also works as a scripted check.
//
// The debug UI runs on top (E7.2, E7.3): F3 toggles the performance overlay,
// F3+Left and F3+Right switch its pages. --overlay-page N opens the overlay on
// page N (for screenshots), --imgui-demo adds the Dear ImGui and ImPlot demo
// windows, --no-ui turns the whole debug UI off.

#include <vxe/config/settings.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/debug/debug_ui.hpp>
#include <vxe/debug/overlay.hpp>
#include <vxe/input/input.hpp>
#include <vxe/math/projection.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/rhi/gpu_timer.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/rhi_gl/gl_device.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <implot.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace vxe;
using namespace vxe::rhi;

// Uniform blocks, laid out like their std140 declarations in the shaders.
struct TriangleParams {
    Vec4 tint{1.0F};
    Vec2 offset{0.0F};
    f32 scale = 1.0F;
    f32 depth = 0.5F;
};
static_assert(sizeof(TriangleParams) == 32);

struct CubeParams {
    Mat4 modelViewProjection{1.0F};
    Mat4 model{1.0F};
};
static_assert(sizeof(CubeParams) == 128);

// Uniform buffer offsets must be multiples of the device alignment, which is
// 256 bytes or less on every GPU the engine targets.
constexpr usize kTriangleParamsOffset = 0;
constexpr usize kCubeParamsOffset = 256;
constexpr usize kUniformBufferSize = 512;

// The cube's 36 indices into its 8 corners (corner bits: x, y, z), wound
// counterclockwise when seen from outside, so back face culling works.
constexpr std::array<u16, 36> kCubeIndices{0, 4, 6, 0, 6, 2, 5, 1, 3, 5, 3, 7, 0, 1, 5, 0, 5, 4,
                                           2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};

struct Options {
    std::optional<u64> frames;
    bool vsync = false;
    bool ui = true;
    bool imguiDemo = false;
    std::optional<u32> overlayPage;
};

std::optional<Options> parseOptions(int argc, char** argv) {
    Options options;
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (usize i = 0; i < args.size(); ++i) {
        if (args[i] == "--vsync") {
            options.vsync = true;
        } else if (args[i] == "--no-ui") {
            options.ui = false;
        } else if (args[i] == "--imgui-demo") {
            options.imguiDemo = true;
        } else if (args[i] == "--overlay-page" && i + 1 < args.size()) {
            u32 page = 0;
            const std::string_view text = args[++i];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), page);
            if (error != std::errc() || end != text.data() + text.size()) {
                return std::nullopt;
            }
            options.overlayPage = page;
        } else if (args[i] == "--frames" && i + 1 < args.size()) {
            u64 frames = 0;
            const std::string_view text = args[++i];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), frames);
            if (error != std::errc() || end != text.data() + text.size() || frames == 0) {
                return std::nullopt;
            }
            options.frames = frames;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

// Every GPU object of the demo, created once (targets again on resize).
struct Scene {
    PipelineHandle cubePipeline;
    PipelineHandle blitPipeline;
    PipelineHandle trianglePipeline;
    BufferHandle uniforms;
    BufferHandle cubeIndices;
    SamplerHandle sampler;
    TextureHandle color;
    TextureHandle depth;
    u32 width = 0;
    u32 height = 0;
};

Result<PipelineHandle> createPipeline(Device& device, const ShaderFileReader& shaders,
                                      std::string_view vertexPath, std::string_view fragmentPath,
                                      PipelineDesc desc) {
    const auto vertex = createShaderFromFile(device, vertexPath, shaders);
    if (!vertex) {
        return vertex.error();
    }
    const auto fragment = createShaderFromFile(device, fragmentPath, shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error();
    }
    desc.vertex = vertex.value();
    desc.fragment = fragment.value();
    auto pipeline = device.createPipeline(desc);
    // The linked program keeps what it needs; the shaders can go right away.
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    return pipeline;
}

Result<void> createTargets(Device& device, Scene& scene, u32 width, u32 height) {
    device.destroy(scene.color);
    device.destroy(scene.depth);
    auto color = device.createTexture({.format = Format::RGBA8Unorm,
                                       .width = width,
                                       .height = height,
                                       .usage = TextureUsage::RenderTarget | TextureUsage::Sampled,
                                       .debugName = "Demo color"});
    if (!color) {
        return color.error();
    }
    auto depth = device.createTexture({.format = Format::D32Float,
                                       .width = width,
                                       .height = height,
                                       .usage = TextureUsage::DepthTarget,
                                       .debugName = "Demo depth"});
    if (!depth) {
        device.destroy(color.value());
        return depth.error();
    }
    scene.color = color.value();
    scene.depth = depth.value();
    scene.width = width;
    scene.height = height;
    return {};
}

Result<void> createScene(Device& device, Scene& scene) {
    const ShaderFileReader shaders = folderShaderReader(VXE_ENGINE_SHADER_DIR);

    PipelineDesc cube{.debugName = "Demo cube"};
    cube.colorFormats[0] = Format::RGBA8Unorm;
    cube.colorCount = 1;
    cube.depthFormat = Format::D32Float;
    auto cubePipeline = createPipeline(device, shaders, "debug/cube.vert", "debug/cube.frag", cube);
    if (!cubePipeline) {
        return cubePipeline.error();
    }
    scene.cubePipeline = cubePipeline.value();

    // The window pass has no depth buffer, so its pipelines test no depth.
    PipelineDesc overlay{.debugName = "Demo blit"};
    overlay.colorFormats[0] = Format::RGBA8Unorm;
    overlay.colorCount = 1;
    overlay.depth.testEnabled = false;
    overlay.depth.writeEnabled = false;
    overlay.raster.cullMode = CullMode::None;
    auto blit = createPipeline(device, shaders, "debug/blit.vert", "debug/blit.frag", overlay);
    if (!blit) {
        return blit.error();
    }
    scene.blitPipeline = blit.value();
    overlay.debugName = "Demo triangle";
    auto triangle = createPipeline(device, shaders, "debug/triangle.vert", "debug/triangle.frag", overlay);
    if (!triangle) {
        return triangle.error();
    }
    scene.trianglePipeline = triangle.value();

    auto uniforms = device.createBuffer({.sizeBytes = kUniformBufferSize,
                                         .usage = BufferUsage::Uniform | BufferUsage::TransferDst,
                                         .debugName = "Demo uniforms"});
    if (!uniforms) {
        return uniforms.error();
    }
    scene.uniforms = uniforms.value();
    auto indices = device.createBuffer({.sizeBytes = sizeof(kCubeIndices),
                                        .usage = BufferUsage::Index | BufferUsage::TransferDst,
                                        .debugName = "Demo cube indices"});
    if (!indices) {
        return indices.error();
    }
    scene.cubeIndices = indices.value();
    VXE_TRY(device.updateBuffer(scene.cubeIndices, 0, std::as_bytes(std::span(kCubeIndices))));
    auto sampler = device.createSampler({.addressU = AddressMode::ClampToEdge,
                                         .addressV = AddressMode::ClampToEdge,
                                         .debugName = "Demo blit sampler"});
    if (!sampler) {
        return sampler.error();
    }
    scene.sampler = sampler.value();
    return createTargets(device, scene, std::max(device.backBufferWidth(), 1U),
                         std::max(device.backBufferHeight(), 1U));
}

void destroyScene(Device& device, Scene& scene) {
    for (const PipelineHandle pipeline : {scene.cubePipeline, scene.blitPipeline, scene.trianglePipeline}) {
        device.destroy(pipeline);
    }
    device.destroy(scene.uniforms);
    device.destroy(scene.cubeIndices);
    device.destroy(scene.sampler);
    device.destroy(scene.color);
    device.destroy(scene.depth);
}

// Records one frame: the cube offscreen, then the window pass with the
// triangle and the debug UI on top.
void recordFrame(CommandList& commands, const Scene& scene, debug::DebugUi* ui) {
    commands.beginDebugGroup("Cube pass");
    RenderPassDesc offscreen;
    offscreen.colorCount = 1;
    offscreen.colors[0] = {.texture = scene.color, .clearColor = {0.05F, 0.06F, 0.09F, 1.0F}};
    offscreen.hasDepth = true;
    offscreen.depth = {.texture = scene.depth, .store = StoreOp::DontCare};
    commands.beginRenderPass(offscreen);
    commands.bindPipeline(scene.cubePipeline);
    commands.bindUniformBuffer(1, 1, scene.uniforms, kCubeParamsOffset, sizeof(CubeParams));
    commands.bindIndexBuffer(scene.cubeIndices, IndexType::Uint16);
    commands.drawIndexed(static_cast<u32>(kCubeIndices.size()));
    commands.endRenderPass();
    commands.endDebugGroup();

    commands.beginDebugGroup("Window pass");
    RenderPassDesc window;
    window.colorCount = 1;
    window.colors[0] = {.swapchain = true, .load = LoadOp::DontCare};
    commands.beginRenderPass(window);
    commands.bindPipeline(scene.blitPipeline);
    commands.bindTexture(2, 0, scene.color, scene.sampler);
    commands.draw(3);
    commands.bindPipeline(scene.trianglePipeline);
    commands.bindUniformBuffer(1, 0, scene.uniforms, kTriangleParamsOffset, sizeof(TriangleParams));
    commands.draw(3);
    if (ui != nullptr) {
        ui->record(commands);
    }
    commands.endRenderPass();
    commands.endDebugGroup();
}

Result<void> updateUniforms(Device& device, const Scene& scene, f64 seconds) {
    const auto time = static_cast<f32>(seconds);
    const f32 aspect = static_cast<f32>(scene.width) / static_cast<f32>(std::max(scene.height, 1U));
    const Mat4 projection = perspectiveReversedInfinite(glm::radians(60.0F), aspect, 0.1F);
    const Mat4 view = cameraRelativeView(Vec3(0.0F, 0.0F, -1.0F));
    Mat4 model = glm::translate(Mat4(1.0F), Vec3(0.0F, 0.0F, -4.0F));
    model = glm::rotate(model, time * 0.9F, glm::normalize(Vec3(0.3F, 1.0F, 0.15F)));
    const CubeParams cube{.modelViewProjection = projection * view * model, .model = model};
    VXE_TRY(device.updateBuffer(scene.uniforms, kCubeParamsOffset, std::as_bytes(std::span(&cube, 1))));

    // A small triangle in the lower left corner, gently pulsing.
    const TriangleParams triangle{.tint = Vec4(1.0F),
                                  .offset = Vec2(-0.72F, -0.68F),
                                  .scale = 0.22F + 0.02F * std::sin(time * 2.0F),
                                  .depth = 0.5F};
    return device.updateBuffer(scene.uniforms, kTriangleParamsOffset, std::as_bytes(std::span(&triangle, 1)));
}

// The debug UI with the performance overlay, and what they read. Only exists
// while the UI is on (not with --no-ui).
struct DebugTools {
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    std::unique_ptr<debug::DebugUi> ui;
    std::unique_ptr<debug::DebugOverlay> overlay;
};

Result<std::unique_ptr<DebugTools>> createDebugTools(Device& device, input::InputSystem& input,
                                                     const Options& options) {
    auto tools = std::make_unique<DebugTools>();
    auto ui = debug::DebugUi::create(device, input, {.shaders = folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    if (!ui) {
        return ui.error();
    }
    tools->ui = std::move(ui.value());
    tools->overlay = std::make_unique<debug::DebugOverlay>(debug::OverlayDesc{
        .settings = &tools->settings, .input = &input, .stats = &tools->stats, .device = &device});
    if (options.overlayPage) {
        tools->overlay->setVisible(true);
        tools->overlay->showPage(*options.overlayPage);
    }
    return tools;
}

// One frame of the debug UI, in the order DebugOverlay documents.
void updateDebugTools(DebugTools& tools, platform::Window& window,
                      std::span<const platform::PlatformEvent> events, const FrameTime& time,
                      std::optional<f64> gpuMilliseconds, bool imguiDemo) {
    tools.ui->processEvents(events);
    tools.overlay->processEvents(events);
    // The frame that just ended: its CPU time, and the newest GPU time the
    // timer has (a few frames old by design).
    tools.overlay->recordFrame(toMilliseconds(time.rawDelta), gpuMilliseconds);
    tools.stats.endFrame(time.deltaSeconds);
    tools.ui->beginFrame(time.deltaSeconds, Vec2(window.size()), Vec2(window.pixelSize()));
    tools.overlay->update(time.deltaSeconds);
    if (imguiDemo) {
        ImGui::ShowDemoWindow();
        ImPlot::ShowDemoWindow();
    }
    tools.ui->endFrame();
    window.setTextInputEnabled(tools.ui->wantsTextInput());
    tools.settings.dispatchChanges();
}

// Shows GPU time, CPU time and frame rate in the window title, averaged over
// half a second so the numbers are readable.
class TitleReadout {
public:
    void update(platform::Window& window, f64 deltaSeconds, std::optional<f64> gpuMilliseconds) {
        m_seconds += deltaSeconds;
        ++m_frames;
        if (m_seconds < 0.5) {
            return;
        }
        window.setTitle(std::format(
            "vxe RHI demo | GPU {} | CPU {:.3f} ms | {:.0f} FPS",
            gpuMilliseconds ? std::format("{:.3f} ms", *gpuMilliseconds) : std::string("n/a"),
            m_seconds * 1000.0 / static_cast<f64>(m_frames), static_cast<f64>(m_frames) / m_seconds));
        m_seconds = 0.0;
        m_frames = 0;
    }

private:
    f64 m_seconds = 0.0;
    u64 m_frames = 0;
};

int run(const Options& options) {
    log::initialize(log::Config{.filePath = {}});
    auto platform = platform::Platform::create();
    if (!platform) {
        VXE_LOG_ERROR(log::core, "{}", platform.error().describe());
        return 1;
    }
#ifdef NDEBUG
    constexpr bool kDebugContext = false;
#else
    constexpr bool kDebugContext = true;
#endif
    auto window = platform::Window::create(
        *platform.value(),
        {.title = "vxe RHI demo", .width = 1280, .height = 720, .debugContext = kDebugContext});
    if (!window) {
        VXE_LOG_ERROR(log::core, "{}", window.error().describe());
        return 1;
    }
    auto created = createGlDevice(*window.value());
    if (!created) {
        VXE_LOG_ERROR(log::core, "{}", created.error().describe());
        return 1;
    }
    Device& device = *created.value();
    device.setPresentMode(options.vsync ? PresentMode::Vsync : PresentMode::Immediate);

    Scene scene;
    GpuFrameTimer gpuTimer;
    if (auto result = createScene(device, scene); !result) {
        VXE_LOG_ERROR(log::core, "{}", result.error().describe());
        destroyScene(device, scene);
        return 1;
    }
    if (device.capabilities().timestampQueries) {
        static_cast<void>(gpuTimer.create(device));
    }
    input::InputSystem input;
    std::unique_ptr<DebugTools> tools;
    if (options.ui) {
        auto toolsCreated = createDebugTools(device, input, options);
        if (!toolsCreated) {
            VXE_LOG_ERROR(log::core, "{}", toolsCreated.error().describe());
            gpuTimer.destroy(device);
            destroyScene(device, scene);
            return 1;
        }
        tools = std::move(toolsCreated.value());
    }

    FrameTimer frameTimer;
    CommandList commands;
    std::vector<platform::PlatformEvent> events;
    bool running = true;
    u64 frames = 0;
    f64 cpuMillisecondsTotal = 0.0;
    TitleReadout title;
    while (running) {
        const FrameTime time = frameTimer.tick();
        events.clear();
        static_cast<void>(platform.value()->pollEvents(events));
        for (const auto& event : events) {
            if (std::holds_alternative<platform::QuitRequested>(event)) {
                running = false;
            }
        }
        input.update(events);
        if (tools) {
            updateDebugTools(*tools, *window.value(), events, time, gpuTimer.lastFrameMilliseconds(),
                             options.imguiDemo);
        }
        const u32 width = std::max(device.backBufferWidth(), 1U);
        const u32 height = std::max(device.backBufferHeight(), 1U);
        if (width != scene.width || height != scene.height) {
            if (auto result = createTargets(device, scene, width, height); !result) {
                VXE_LOG_ERROR(log::core, "{}", result.error().describe());
                break;
            }
        }
        if (auto result = updateUniforms(device, scene, time.elapsedSeconds); !result) {
            VXE_LOG_ERROR(log::core, "{}", result.error().describe());
            break;
        }

        commands.reset();
        gpuTimer.beginFrame(device, commands);
        recordFrame(commands, scene, tools ? tools->ui.get() : nullptr);
        gpuTimer.endFrame(commands);
        device.beginFrame();
        device.submit(commands);
        device.present();

        ++frames;
        if (frames > 1) {
            cpuMillisecondsTotal += time.deltaSeconds * 1000.0;
        }
        title.update(*window.value(), time.deltaSeconds, gpuTimer.lastFrameMilliseconds());
        if (options.frames && frames >= *options.frames) {
            running = false;
        }
    }

    const DeviceStats stats = device.stats();
    const auto gpu = gpuTimer.lastFrameMilliseconds();
    VXE_LOG_INFO(log::core,
                 "{} frames, CPU {:.3f} ms per frame, GPU {} per frame, {} draw calls, {} invalid commands, "
                 "{} driver errors",
                 frames, frames > 1 ? cpuMillisecondsTotal / static_cast<f64>(frames - 1) : 0.0,
                 gpu ? std::format("{:.3f} ms", *gpu) : std::string("not measured"), stats.drawCalls,
                 stats.invalidCommands, stats.apiErrors);
    tools.reset();
    gpuTimer.destroy(device);
    destroyScene(device, scene);
    // The leak report (E6.6): everything the demo created must be gone now.
    const std::vector<std::string> leaks = device.liveResources();
    for (const std::string& leak : leaks) {
        VXE_LOG_ERROR(log::core, "Leaked {}", leak);
    }
    created.value().reset();
    log::shutdown();
    return stats.invalidCommands == 0 && stats.apiErrors == 0 && leaks.empty() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        if (!options) {
            std::fputs(
                "Usage: vxe_rhi_demo [--frames N] [--vsync] [--no-ui] [--imgui-demo] [--overlay-page N]\n",
                stderr);
            return 2;
        }
        return run(*options);
    } catch (const std::exception& error) {
        // fputs, because streams can throw again while reporting.
        std::fputs("vxe_rhi_demo: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
    } catch (...) {
        std::fputs("vxe_rhi_demo: unknown failure\n", stderr);
    }
    return 3;
}
