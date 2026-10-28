#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/frame_constants.hpp>
#include <vxe/render/sky.hpp>
#include <vxe/rhi_gl/gl_device.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <vector>

using namespace vxe;

namespace {

std::unique_ptr<render::SkyRenderer> makeSky(rhi::Device& device, config::SettingsRegistry& settings,
                                             stats::StatRegistry* stats = nullptr) {
    auto sky = render::SkyRenderer::create({.device = &device,
                                            .settings = &settings,
                                            .stats = stats,
                                            .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    REQUIRE(sky.hasValue());
    return std::move(sky.value());
}

} // namespace

TEST_CASE("The sun follows the time of day and tints the sky") {
    const auto noon = render::computeSkyParams({.hours = 12.0F});
    const auto midnight = render::computeSkyParams({.hours = 0.0F});
    const auto sunrise = render::computeSkyParams({.hours = 6.0F});
    const auto evening = render::computeSkyParams({.hours = 18.25F});
    CHECK(noon.sunDirection.y > 0.85F);
    CHECK(midnight.sunDirection.y < -0.85F);
    CHECK(sunrise.sunDirection.y == doctest::Approx(0.0F).epsilon(1e-5));
    CHECK(sunrise.sunDirection.x == doctest::Approx(1.0F)); // Rises in the east (+X).
    // Blue day, dark night, orange horizon at sunset.
    CHECK(noon.zenithColor.b > noon.zenithColor.r);
    CHECK(midnight.zenithColor.b < 0.05F);
    CHECK(evening.horizonColor.r > evening.horizonColor.b);
    // No sun disk below the horizon.
    CHECK(midnight.sunColor.r == 0.0F);
    const auto noFog = render::computeSkyParams({.fogEnabled = false});
    CHECK(noFog.flags.y == 0);
    CHECK(noFog.flags.x == 1);
}

TEST_CASE("Sky settings apply live and the day cycle runs") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    auto sky = makeSky(*device, settings, &stats);
    sky->update(0.0);
    CHECK(sky->state().hours == 10.0F);

    REQUIRE(settings.set("Graphics.Sky.TimeOfDay", 20.0F).hasValue());
    sky->update(0.016);
    CHECK(sky->state().hours == 20.0F);

    // One real minute per day: 30 seconds is half a day, wrapping past midnight.
    REQUIRE(settings.set("Graphics.Sky.DayLengthMinutes", 1.0F).hasValue());
    sky->update(30.0);
    CHECK(sky->state().hours == doctest::Approx(8.0F));
    // Setting the time by hand restarts the cycle from there.
    REQUIRE(settings.set("Graphics.Sky.TimeOfDay", 12.0F).hasValue());
    sky->update(0.0);
    CHECK(sky->state().hours == 12.0F);

    REQUIRE(settings.set("Graphics.Sky.FogDensity", 0.01F).hasValue());
    REQUIRE(settings.set("Graphics.Sky.Enabled", false).hasValue());
    sky->update(0.0);
    CHECK(sky->params().fog.y == 0.01F);
    CHECK(sky->params().flags.x == 0);
    stats.endFrame(1.0 / 60.0);
    CHECK(stats.value(stats.find("Render.Sky.TimeOfDay")) == 12.0);
    CHECK(stats.value(stats.find("Render.Sky.SunElevation")) > 50.0);
}

namespace {

// Draws the sky pass over a red scene whose depth is all `depth`, and returns
// the pixels (RGBA8, bottom row first). `pitch` tilts the camera up.
std::vector<std::byte> renderSky(rhi::Device& device, render::SkyRenderer& sky, f32 depth, f32 pitch) {
    constexpr u32 kSize = 64;
    const auto color =
        device.createTexture({.width = kSize,
                              .height = kSize,
                              .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
    const auto depthTexture =
        device.createTexture({.format = rhi::Format::D32Float,
                              .width = kSize,
                              .height = kSize,
                              .usage = rhi::TextureUsage::DepthTarget | rhi::TextureUsage::Sampled});
    REQUIRE(color.hasValue());
    REQUIRE(depthTexture.hasValue());
    render::Camera camera;
    camera.position = {0.5, 70.0, 0.5};
    camera.pitch = pitch;
    render::FrameConstantsBuffer constants;
    REQUIRE(constants.create(device).hasValue());
    constants.update(device, render::makeFrameConstants(camera, {.width = kSize, .height = kSize}));
    sky.update(0.0);

    rhi::CommandList commands;
    rhi::RenderPassDesc scene; // The "scene": red, at the given depth.
    scene.colorCount = 1;
    scene.colors[0] = {.texture = color.value(), .clearColor = {1.0F, 0.0F, 0.0F, 1.0F}};
    scene.hasDepth = true;
    scene.depth = {.texture = depthTexture.value(), .clearDepth = depth};
    commands.beginRenderPass(scene);
    commands.endRenderPass();
    rhi::RenderPassDesc skyPass;
    skyPass.colorCount = 1;
    skyPass.colors[0] = {.texture = color.value(), .load = rhi::LoadOp::Load};
    commands.beginRenderPass(skyPass);
    constants.bind(commands);
    sky.record(commands, depthTexture.value());
    commands.endRenderPass();
    device.beginFrame();
    device.submit(commands);
    device.present();
    std::vector<std::byte> pixels(std::size_t{kSize} * kSize * 4);
    REQUIRE(device.readTexture(color.value(), 0, 0, pixels).hasValue());
    device.destroy(color.value());
    device.destroy(depthTexture.value());
    constants.destroy(device);
    return pixels;
}

} // namespace

TEST_CASE("The sky and fog pass draws sky and fogs geometry (VXE_GUI_TESTS=1)") {
    if (!test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = platform::Platform::create();
    REQUIRE(platform.hasValue());
    auto window = platform::Window::create(*platform.value(),
                                           {.title = "vxe sky test", .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto created = rhi::createGlDevice(*window.value(), {.debugOutput = true});
    REQUIRE(created.hasValue());
    rhi::Device& device = *created.value();
    {
        config::SettingsRegistry settings;
        auto sky = makeSky(device, settings);
        REQUIRE(settings.set("Graphics.Sky.TimeOfDay", 12.0F).hasValue());
        const auto pixel = [](const std::vector<std::byte>& pixels, std::size_t x, std::size_t y,
                              std::size_t c) { return static_cast<int>(pixels[(y * 64 + x) * 4 + c]); };
        // Nothing drawn (depth 0): noon sky, blue at the top.
        const auto open = renderSky(device, *sky, 0.0F, 0.3F);
        MESSAGE("Sky top pixel " << pixel(open, 32, 63, 0) << " " << pixel(open, 32, 63, 1) << " "
                                 << pixel(open, 32, 63, 2));
        CHECK(pixel(open, 32, 63, 2) > pixel(open, 32, 63, 0) + 60);

        // Geometry 500 blocks away (reversed Z depth = near / distance) seen
        // along the horizon, near the fog's base height, in thick fog: the
        // red scene turns into the light horizon color. (Looking up, the ray
        // would leave the height fog and keep more of the scene.)
        REQUIRE(settings.set("Graphics.Sky.FogDensity", 0.01F).hasValue());
        const auto foggy = renderSky(device, *sky, 0.05F / 500.0F, 0.0F);
        MESSAGE("Fogged pixel " << pixel(foggy, 32, 32, 0) << " " << pixel(foggy, 32, 32, 1) << " "
                                << pixel(foggy, 32, 32, 2));
        // Linear horizon color (the post pass encodes it later): light blue.
        CHECK(pixel(foggy, 32, 32, 2) > 180);
        CHECK(pixel(foggy, 32, 32, 1) > pixel(foggy, 32, 32, 0));

        // Fog off, live: the scene stays red.
        REQUIRE(settings.set("Graphics.Sky.FogEnabled", false).hasValue());
        const auto clear = renderSky(device, *sky, 0.05F / 500.0F, 0.0F);
        CHECK(pixel(clear, 32, 32, 0) == 255);
        CHECK(pixel(clear, 32, 32, 1) == 0);
        CHECK(device.stats().apiErrors == 0);
        CHECK(device.stats().invalidCommands == 0);
    }
    device.waitIdle();
    CHECK(device.liveResources().empty());
}
