// vxe_rhi_demo: a spinning cube and a colored triangle drawn through the RHI
// (Section 7, the E6.5 acceptance demo).
//
// Usage: vxe_rhi_demo [--frames N] [--vsync] [--no-ui] [--imgui-demo]
//                     [--overlay-page N] [--console] [--exec COMMAND]...
//                     [--origin BLOCKS]
//
// The cube is drawn into an offscreen color and depth target with reversed Z,
// then copied to the window with a fullscreen triangle, and the triangle is
// drawn on top. The passes run in a frame graph (E8.1), which owns and
// resizes the offscreen targets and times every pass on the GPU.
//
// The scene is flown with a free camera (E8.2): W, A, S, D, Space and Left
// Shift move, Left Ctrl is fast, the right mouse button looks, the wheel
// changes the speed. A debug grid lies under the start position and F3+G
// shows chunk borders. The sky, sun and height fog (E8.3) follow the
// Graphics.Sky settings live, for example --exec "set Graphics.Sky.TimeOfDay 18"
// or a running day with Graphics.Sky.DayLengthMinutes. --origin N starts the camera N blocks from the world
// origin on X and Z, to check that nothing shakes far away (try 10000000). The window title shows the GPU
// frame time (measured with timestamp queries, without stalls), the CPU frame time and the frame rate. With
// --frames the demo stops after N frames and prints a summary; the exit code is non zero if the device
// reported invalid commands or driver errors, so the demo also works as a scripted check.
//
// The debug UI runs on top (E7.2, E7.3): F3 toggles the performance overlay,
// F3+Left and F3+Right switch its pages. --overlay-page N opens the overlay on
// page N (for screenshots), --imgui-demo adds the Dear ImGui and ImPlot demo
// windows, --no-ui turns the whole debug UI off. The Grave key opens the
// developer console (E7.4), --console opens it at startup, and --exec runs a
// console command at startup, for
// example --exec "set Video.FpsLimit 144" (Video.FpsLimit caps the frame rate).

#include <vxe/config/console.hpp>
#include <vxe/config/settings.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/debug/debug_ui.hpp>
#include <vxe/debug/developer_console.hpp>
#include <vxe/debug/overlay.hpp>
#include <vxe/input/input.hpp>
#include <vxe/math/projection.hpp>
#include <vxe/platform/frame_limiter.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/frame_constants.hpp>
#include <vxe/render/frame_graph.hpp>
#include <vxe/render/sky.hpp>
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
    std::vector<std::string> commands; // From --exec, run through the console at startup.
    bool openConsole = false;
    f64 origin = 0.0; // Where the camera starts on X and Z, in blocks.
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
        } else if (args[i] == "--console") {
            options.openConsole = true;
        } else if (args[i] == "--origin" && i + 1 < args.size()) {
            const std::string_view text = args[++i];
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), options.origin);
            if (error != std::errc() || end != text.data() + text.size()) {
                return std::nullopt;
            }
        } else if (args[i] == "--exec" && i + 1 < args.size()) {
            options.commands.emplace_back(args[++i]);
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
    u32 width = 1; // The window size, for the projection's aspect ratio.
    u32 height = 1;
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
    return {};
}

void destroyScene(Device& device, Scene& scene) {
    for (const PipelineHandle pipeline : {scene.cubePipeline, scene.blitPipeline, scene.trianglePipeline}) {
        device.destroy(pipeline);
    }
    device.destroy(scene.uniforms);
    device.destroy(scene.cubeIndices);
    device.destroy(scene.sampler);
}

// What the free camera sees (E8.2): the camera and its controller, debug
// lines (the grid and chunk borders) and the frame constants they need.
struct WorldView {
    render::Camera camera;
    std::unique_ptr<render::FreeCameraController> controller;
    std::unique_ptr<render::DebugDraw> debugDraw;
    std::unique_ptr<render::SkyRenderer> sky;
    render::FrameConstantsBuffer constants;
    DVec3 start{0.0};
    bool looking = false;
};

constexpr f64 kGridHeight = 64.0;

Result<std::unique_ptr<WorldView>> createWorldView(Device& device, config::SettingsRegistry& settings,
                                                   stats::StatRegistry& statRegistry,
                                                   input::InputSystem& input,
                                                   input::InputContextId debugContext, f64 origin) {
    auto view = std::make_unique<WorldView>();
    view->start = DVec3(origin + 8.5, kGridHeight + 4.0, origin + 8.5);
    view->camera.position = view->start;
    view->camera.pitch = glm::radians(-20.0F);
    view->controller = std::make_unique<render::FreeCameraController>(
        render::FreeCameraDesc{.input = &input, .settings = &settings});
    auto draw = render::DebugDraw::create({.device = &device,
                                           .settings = &settings,
                                           .stats = &statRegistry,
                                           .input = &input,
                                           .toggleContext = debugContext,
                                           .shaders = folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    if (!draw) {
        return draw.error();
    }
    view->debugDraw = std::move(draw.value());
    auto sky = render::SkyRenderer::create({.device = &device,
                                            .settings = &settings,
                                            .stats = &statRegistry,
                                            .shaders = folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    if (!sky) {
        return sky.error();
    }
    view->sky = std::move(sky.value());
    VXE_TRY(view->constants.create(device));
    return view;
}

// One frame of flying: input, the grid under the start position, the frame
// constants. The mouse is captured only while the right button looks around.
void updateWorldView(WorldView& view, Device& device, platform::Window& window, const FrameTime& time,
                     u64 frameIndex) {
    view.controller->update(view.camera, time.deltaSeconds);
    if (view.controller->isLooking() != view.looking) {
        view.looking = view.controller->isLooking();
        static_cast<void>(window.setRelativeMouseMode(view.looking));
    }
    view.sky->update(time.deltaSeconds);
    render::DebugDraw& draw = *view.debugDraw;
    draw.handleInput();
    draw.beginFrame(view.camera);
    constexpr u32 kMinor = render::debugColor(90, 96, 110, 160);
    constexpr u32 kMajor = render::debugColor(90, 150, 255, 220);
    draw.grid(view.camera.position, kGridHeight, 96, 1.0, kMinor, kMajor, 16);
    // An outline around the cube, which floats in front of the start.
    draw.box(view.start + DVec3(-1.1, -1.1, -9.1), view.start + DVec3(1.1, 1.1, -6.9),
             render::debugColor(255, 220, 60));
    const u32 width = std::max(device.backBufferWidth(), 1U);
    const u32 height = std::max(device.backBufferHeight(), 1U);
    view.constants.update(device, render::makeFrameConstants(view.camera, {.width = width,
                                                                           .height = height,
                                                                           .seconds = time.elapsedSeconds,
                                                                           .deltaSeconds = time.deltaSeconds,
                                                                           .frameIndex = frameIndex}));
}

// The demo's frame as a frame graph (E8.1): the cube into offscreen targets
// the graph creates and resizes, then the window with the cube image and the
// triangle, then the debug UI on top. Each pass shows up with its GPU time on
// the overlay's GPU page and can be switched off with Debug.Passes.<name>.
Result<void> buildFrameGraph(render::FrameGraph& graph, const Scene& scene, WorldView& view,
                             debug::DebugUi* ui) {
    auto cube = graph.addPass({.name = "Cube", .record = [&scene](render::PassContext& context) {
                                   context.commands.bindPipeline(scene.cubePipeline);
                                   context.commands.bindUniformBuffer(1, 1, scene.uniforms, kCubeParamsOffset,
                                                                      sizeof(CubeParams));
                                   context.commands.bindIndexBuffer(scene.cubeIndices, IndexType::Uint16);
                                   context.commands.drawIndexed(static_cast<u32>(kCubeIndices.size()));
                               }});
    const render::TextureRef color = cube.createTexture("SceneColor", {.format = Format::RGBA8Unorm});
    cube.colorAttachment(color, LoadOp::Clear, {0.05F, 0.06F, 0.09F, 1.0F});
    const render::TextureRef depth = cube.createTexture("SceneDepth", {.format = Format::D32Float});
    cube.depthAttachment(depth);

    // Sky where nothing was drawn and height fog over the rest (E8.3).
    auto skyPass = graph.addPass({.name = "Sky", .record = [&view, depth](render::PassContext& context) {
                                      view.constants.bind(context.commands);
                                      view.sky->record(context.commands, context.texture(depth));
                                  }});
    skyPass.colorAttachment(color, LoadOp::Load);
    skyPass.sampleTexture(depth);

    // Debug lines on top of the cube, hidden by it where it is in front.
    auto lines = graph.addPass({.name = "DebugDraw", .record = [&view](render::PassContext& context) {
                                    view.constants.bind(context.commands);
                                    view.debugDraw->record(context.commands);
                                }});
    lines.colorAttachment(color, LoadOp::Load);
    lines.depthAttachment(depth, LoadOp::Load, 0.0F, StoreOp::DontCare);

    auto composite =
        graph.addPass({.name = "Composite", .record = [&scene, color](render::PassContext& context) {
                           context.commands.bindPipeline(scene.blitPipeline);
                           context.commands.bindTexture(2, 0, context.texture(color), scene.sampler);
                           context.commands.draw(3);
                           context.commands.bindPipeline(scene.trianglePipeline);
                           context.commands.bindUniformBuffer(1, 0, scene.uniforms, kTriangleParamsOffset,
                                                              sizeof(TriangleParams));
                           context.commands.draw(3);
                       }});
    composite.sampleTexture(color);
    composite.windowAttachment(LoadOp::DontCare);

    if (ui != nullptr) {
        // Not switchable: without it there is no way to switch passes back on.
        graph
            .addPass({.name = "DebugUi",
                      .canDisable = false,
                      .record = [ui](render::PassContext& context) { ui->record(context.commands); }})
            .windowAttachment(LoadOp::Load);
    }
    return graph.compile();
}

Result<void> updateUniforms(Device& device, const Scene& scene, const WorldView& view, f64 seconds) {
    const auto time = static_cast<f32>(seconds);
    const f32 aspect = static_cast<f32>(scene.width) / static_cast<f32>(std::max(scene.height, 1U));
    // Camera relative model matrix: the translation is made in double.
    Mat4 model = glm::translate(Mat4(1.0F), view.camera.relative(view.start + DVec3(0.0, 0.0, -8.0)));
    model = glm::rotate(model, time * 0.9F, glm::normalize(Vec3(0.3F, 1.0F, 0.15F)));
    const CubeParams cube{.modelViewProjection = view.camera.viewProjection(aspect) * model, .model = model};
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
    stats::StatRegistry* stats = nullptr;
    config::CommandRegistry commands;
    std::unique_ptr<debug::DebugUi> ui;
    std::unique_ptr<debug::DebugOverlay> overlay;
    std::unique_ptr<debug::DeveloperConsole> console;
};

Result<std::unique_ptr<DebugTools>> createDebugTools(Device& device, input::InputSystem& input,
                                                     config::SettingsRegistry& settings,
                                                     stats::StatRegistry& statRegistry,
                                                     const Options& options) {
    auto tools = std::make_unique<DebugTools>();
    tools->stats = &statRegistry;
    auto ui = debug::DebugUi::create(device, input, {.shaders = folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    if (!ui) {
        return ui.error();
    }
    tools->ui = std::move(ui.value());
    tools->overlay = std::make_unique<debug::DebugOverlay>(debug::OverlayDesc{
        .settings = &settings, .input = &input, .stats = &statRegistry, .device = &device});
    config::registerSettingsCommands(tools->commands, settings);
    tools->console = std::make_unique<debug::DeveloperConsole>(
        debug::ConsoleDesc{.commands = &tools->commands,
                           .settings = &settings,
                           .input = &input,
                           .stats = &statRegistry,
                           .toggleContext = tools->overlay->inputContext()});
    tools->console->setOpen(options.openConsole);
    if (options.overlayPage) {
        tools->overlay->setVisible(true);
        tools->overlay->showPage(*options.overlayPage);
    }
    return tools;
}

// Runs the --exec commands and adds the camera line to the overview, once
// everything registered its settings, so --exec can change any of them.
void startDebugTools(DebugTools& tools, const WorldView& view, const Options& options) {
    for (const std::string& command : options.commands) {
        static_cast<void>(tools.console->execute(command));
    }
    tools.overlay->addOverviewLine([&camera = view.camera] {
        const CameraPosition position = camera.renderPosition();
        return std::format("XYZ {:.2f} {:.2f} {:.2f}   chunk {} {} {}", camera.position.x, camera.position.y,
                           camera.position.z, position.chunk.x, position.chunk.y, position.chunk.z);
    });
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
    tools.stats->endFrame(time.deltaSeconds);
    tools.ui->beginFrame(time.deltaSeconds, Vec2(window.size()), Vec2(window.pixelSize()));
    tools.overlay->update(time.deltaSeconds);
    tools.console->update();
    if (imguiDemo) {
        ImGui::ShowDemoWindow();
        ImPlot::ShowDemoWindow();
    }
    tools.ui->endFrame();
    window.setTextInputEnabled(tools.ui->wantsTextInput());
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
    // Video.FpsLimit belongs to whoever runs the frame loop; here, the demo.
    config::SettingsRegistry settings;
    const config::Setting<i32> fpsLimit =
        settings.registerInt({{.id = "Video.FpsLimit",
                               .displayName = "FPS limit",
                               .description = "Frames per second cap. 0 for none."},
                              0,
                              0,
                              1000,
                              1});
    platform::FrameLimiter limiter;
    stats::StatRegistry statRegistry;
    input::InputSystem input;
    std::unique_ptr<DebugTools> tools;
    if (options.ui) {
        auto toolsCreated = createDebugTools(device, input, settings, statRegistry, options);
        if (!toolsCreated) {
            VXE_LOG_ERROR(log::core, "{}", toolsCreated.error().describe());
            gpuTimer.destroy(device);
            destroyScene(device, scene);
            return 1;
        }
        tools = std::move(toolsCreated.value());
    }
    auto viewCreated =
        createWorldView(device, settings, statRegistry, input,
                        tools ? tools->overlay->inputContext() : input::InputContextId{}, options.origin);
    std::unique_ptr<WorldView> view = viewCreated ? std::move(viewCreated.value()) : nullptr;
    auto graph = std::make_unique<render::FrameGraph>(
        render::FrameGraphDesc{.device = &device, .settings = &settings, .stats = &statRegistry});
    const Result<void> built = view ? buildFrameGraph(*graph, scene, *view, tools ? tools->ui.get() : nullptr)
                                    : Result<void>(viewCreated.error());
    if (!built) {
        VXE_LOG_ERROR(log::core, "{}", built.error().describe());
        graph.reset();
        if (view) {
            view->constants.destroy(device);
        }
        view.reset();
        tools.reset();
        gpuTimer.destroy(device);
        destroyScene(device, scene);
        return 1;
    }

    if (tools) {
        startDebugTools(*tools, *view, options);
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
        } else {
            statRegistry.endFrame(time.deltaSeconds);
        }
        scene.width = std::max(device.backBufferWidth(), 1U);
        scene.height = std::max(device.backBufferHeight(), 1U);
        graph->setOutputSize(scene.width, scene.height);
        updateWorldView(*view, device, *window.value(), time, frames);
        if (auto result = updateUniforms(device, scene, *view, time.elapsedSeconds); !result) {
            VXE_LOG_ERROR(log::core, "{}", result.error().describe());
            break;
        }

        commands.reset();
        gpuTimer.beginFrame(device, commands);
        if (auto result = graph->execute(commands); !result) {
            VXE_LOG_ERROR(log::core, "{}", result.error().describe());
            break;
        }
        gpuTimer.endFrame(commands);
        device.beginFrame();
        device.submit(commands);
        device.present();
        settings.dispatchChanges();
        // Read every frame, so a change from the console applies at once.
        limiter.wait(static_cast<f64>(fpsLimit.get()));

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
    graph.reset();
    view->constants.destroy(device);
    view.reset();
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
                "Usage: vxe_rhi_demo [--frames N] [--vsync] [--no-ui] [--imgui-demo] [--overlay-page N] "
                "[--console] [--exec COMMAND]... [--origin BLOCKS]\n",
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
