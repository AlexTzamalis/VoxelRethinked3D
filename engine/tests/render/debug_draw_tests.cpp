#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/memory.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/input/input.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/frame_constants.hpp>
#include <vxe/rhi_gl/gl_device.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>
#include <glm/trigonometric.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

using namespace vxe;
using render::Camera;
using render::DebugDraw;

namespace {

constexpr u32 kWhite = render::debugColor(255, 255, 255);
constexpr u32 kRed = render::debugColor(255, 0, 0);

std::unique_ptr<DebugDraw> makeDraw(rhi::Device& device, config::SettingsRegistry& settings,
                                    stats::StatRegistry* stats = nullptr, input::InputSystem* input = nullptr,
                                    input::InputContextId toggle = {}, u32 maxLines = 65536) {
    auto draw = DebugDraw::create({.device = &device,
                                   .settings = &settings,
                                   .stats = stats,
                                   .input = input,
                                   .toggleContext = toggle,
                                   .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR),
                                   .maxLines = maxLines});
    REQUIRE(draw.hasValue());
    return std::move(draw.value());
}

// The test scene: a grid under the camera, a box in front of it and the
// chunk borders, all placed relative to `origin`.
void drawScene(DebugDraw& draw, const Camera& camera, const DVec3& origin) {
    draw.beginFrame(camera);
    draw.grid(camera.position, origin.y, 16, 1.0, kWhite, kRed, 16);
    draw.box(origin + DVec3(-1.0, 1.0, -8.0), origin + DVec3(1.0, 3.0, -6.0), kRed);
}

Camera cameraAt(const DVec3& origin) {
    Camera camera;
    camera.position = origin + DVec3(0.25, 2.5, 0.75);
    camera.yaw = glm::radians(20.0F);
    camera.pitch = glm::radians(-25.0F);
    return camera;
}

} // namespace

TEST_CASE("Debug lines are identical next to the origin and 10 million blocks away") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    REQUIRE(settings.set("Debug.Draw.ChunkBorders", true).hasValue() == false); // Not registered yet.
    auto draw = makeDraw(*device, settings);
    REQUIRE(settings.set("Debug.Draw.ChunkBorders", true).hasValue());

    // Chunk aligned origins, so the chunk borders line up too.
    const DVec3 home(0.0, 64.0, 0.0);
    const DVec3 far(1.0e7, 64.0, -1.0e7);
    drawScene(*draw, cameraAt(home), home);
    const std::vector<render::LineVertex> near(draw->vertices().begin(), draw->vertices().end());
    drawScene(*draw, cameraAt(far), far);
    const std::span<const render::LineVertex> distant = draw->vertices();
    REQUIRE(near.size() == distant.size());
    REQUIRE(!near.empty());
    // Bit for bit: the camera relative positions do not depend on where the
    // camera is in the world. Floats of the absolute positions would be off
    // by up to half a block at this distance (float steps are 1.0 there).
    CHECK(std::memcmp(near.data(), distant.data(), near.size() * sizeof(render::LineVertex)) == 0);
    CHECK(static_cast<f32>(1.0e7 + 0.25) == 1.0e7F);
}

TEST_CASE("Debug draw shapes, chunk borders and the line limit") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    input::InputSystem input;
    const input::InputContextId debug = input.registerContext({.name = "Debug", .alwaysActive = true});
    input.pushContext(debug);
    auto draw = makeDraw(*device, settings, &stats, &input, debug, 64);

    Camera camera;
    camera.position = {8.5, 70.0, 8.5}; // Chunk (0, 4, 0).
    draw->beginFrame(camera);
    draw->box({0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, kWhite);
    CHECK(draw->lineCount() == 12);
    // Edges join corners: every vertex is a corner of the unit box, relative to the camera.
    for (const render::LineVertex& vertex : draw->vertices()) {
        const Vec3 corner = vertex.position + Vec3(8.5F, 70.0F, 8.5F);
        CHECK((corner.x == 0.0F || corner.x == 1.0F));
        CHECK((corner.y == 0.0F || corner.y == 1.0F));
    }

    // F3+G turns the chunk borders on: the camera's chunk (12 edges) and
    // corner lines for a radius of 2 chunks (6 x 6 corners).
    const auto press = [&input](std::vector<platform::PlatformEvent> events) { input.update(events); };
    press({platform::KeyEvent{.scancode = 60, .down = true}});
    press({platform::KeyEvent{.scancode = 10, .down = true}});
    draw->handleInput();
    CHECK(std::get<bool>(settings.value("Debug.Draw.ChunkBorders").value()));
    draw->beginFrame(camera);
    CHECK(draw->lineCount() == 12 + 36);

    // 64 lines at most: a 41 line grid (82 lines) loses 82 + 48 - 64.
    draw->grid(camera.position, 64.0, 20, 1.0, kWhite, kRed, 0);
    CHECK(draw->lineCount() == 64);
    CHECK(draw->droppedLines() == 82 + 48 - 64);
    rhi::CommandList commands;
    draw->record(commands);
    stats.endFrame(1.0 / 60.0);
    CHECK(stats.value(stats.find("Render.DebugDraw.Lines")) == 64.0);
    CHECK(stats.value(stats.find("Render.DebugDraw.Dropped")) == 66.0);
}

TEST_CASE("Recording debug lines does not allocate") {
    if (!memory::globalAllocationCountAvailable()) {
        MESSAGE("Skipped: allocation counting is off in sanitizer builds");
        return;
    }
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto draw = makeDraw(*device, settings);
    rhi::CommandList commands;
    const Camera camera = cameraAt({0.0, 64.0, 0.0});
    for (int i = 0; i < 4; ++i) { // Warm up the command list.
        commands.reset();
        drawScene(*draw, camera, {0.0, 64.0, 0.0});
        draw->record(commands);
        device->submit(commands);
    }
    const u64 before = memory::globalAllocationCount();
    for (int i = 0; i < 100; ++i) {
        commands.reset();
        drawScene(*draw, camera, {0.0, 64.0, 0.0});
        draw->record(commands);
        device->submit(commands);
    }
    CHECK(memory::globalAllocationCount() - before == 0);
}

// E8.2 acceptance on the GPU: the same scene rendered with the camera at the
// origin and 10 million blocks away gives the same pixels.
TEST_CASE("Debug lines render the same far from the origin (VXE_GUI_TESTS=1)") {
    if (!test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = platform::Platform::create();
    REQUIRE(platform.hasValue());
    auto window = platform::Window::create(
        *platform.value(), {.title = "vxe debug draw test", .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto created = rhi::createGlDevice(*window.value(), {.debugOutput = true});
    REQUIRE(created.hasValue());
    rhi::Device& device = *created.value();
    {
        constexpr u32 kSize = 256;
        config::SettingsRegistry settings;
        auto draw = makeDraw(device, settings);
        render::FrameConstantsBuffer constants;
        REQUIRE(constants.create(device).hasValue());
        const auto color =
            device.createTexture({.width = kSize,
                                  .height = kSize,
                                  .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc,
                                  .debugName = "Debug draw test color"});
        const auto depth = device.createTexture({.format = rhi::Format::D32Float,
                                                 .width = kSize,
                                                 .height = kSize,
                                                 .usage = rhi::TextureUsage::DepthTarget,
                                                 .debugName = "Debug draw test depth"});
        REQUIRE(color.hasValue());
        REQUIRE(depth.hasValue());
        const auto render = [&](const DVec3& origin) {
            const Camera camera = cameraAt(origin);
            constants.update(device, render::makeFrameConstants(camera, {.width = kSize, .height = kSize}));
            drawScene(*draw, camera, origin);
            rhi::RenderPassDesc pass;
            pass.colorCount = 1;
            pass.colors[0] = {.texture = color.value(), .clearColor = {0.0F, 0.0F, 0.0F, 1.0F}};
            pass.hasDepth = true;
            pass.depth = {.texture = depth.value()};
            rhi::CommandList commands;
            commands.beginRenderPass(pass);
            constants.bind(commands);
            draw->record(commands);
            commands.endRenderPass();
            device.beginFrame();
            device.submit(commands);
            device.present();
            std::vector<std::byte> pixels(std::size_t{kSize} * kSize * 4);
            REQUIRE(device.readTexture(color.value(), 0, 0, pixels).hasValue());
            return pixels;
        };
        // The first frame after creating the pipelines differs from later
        // identical frames in a few pixels where overlapping lines converge
        // near the horizon (seen on NVIDIA 610.62), whatever the camera
        // position. So the comparison starts with the second frame.
        static_cast<void>(render({0.0, 64.0, 0.0}));
        const auto home = render({0.0, 64.0, 0.0});
        const auto far = render({1.0e7, 64.0, -1.0e7});
        std::size_t lit = 0;
        for (std::size_t i = 0; i < home.size(); i += 4) {
            lit += home[i] != std::byte{0} ? 1U : 0U;
        }
        MESSAGE("Lit pixels: " << lit);
        CHECK(lit > 1000);
        CHECK(home == far);
        CHECK(device.stats().apiErrors == 0);
        CHECK(device.stats().invalidCommands == 0);
        device.destroy(color.value());
        device.destroy(depth.value());
        constants.destroy(device);
    }
    device.waitIdle();
    CHECK(device.liveResources().empty());
}
