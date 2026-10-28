#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/post_process.hpp>
#include <vxe/rhi_gl/gl_device.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <vector>

using namespace vxe;
using render::Tonemapper;

TEST_CASE("Tonemapping curves and sRGB encoding") {
    CHECK(render::tonemap(Vec3(4.0F), Tonemapper::None, 1.0F).x == 1.0F);
    CHECK(render::tonemap(Vec3(0.25F), Tonemapper::None, 2.0F).x == 0.5F); // Exposure first.
    CHECK(render::tonemap(Vec3(1.0F), Tonemapper::Reinhard, 1.0F).x == doctest::Approx(0.5F));
    CHECK(render::tonemap(Vec3(0.0F), Tonemapper::Aces, 1.0F).x == doctest::Approx(0.0F));
    CHECK(render::tonemap(Vec3(1.0F), Tonemapper::Aces, 1.0F).x == doctest::Approx(0.6733F).epsilon(1e-3));
    CHECK(render::tonemap(Vec3(100.0F), Tonemapper::Aces, 1.0F).x == doctest::Approx(1.0F).epsilon(0.01));
    CHECK(render::encodeSrgb(0.5F) == doctest::Approx(0.7354F).epsilon(1e-3));
    CHECK(render::encodeSrgb(0.001F) == doctest::Approx(0.01292F));
    for (const f32 value : {0.0F, 0.002F, 0.18F, 0.5F, 1.0F}) {
        CHECK(render::decodeSrgb(render::encodeSrgb(value)) == doctest::Approx(value).epsilon(1e-4));
    }
}

TEST_CASE("Post processing tonemaps the HDR scene into the window format (VXE_GUI_TESTS=1)") {
    if (!test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = platform::Platform::create();
    REQUIRE(platform.hasValue());
    auto window = platform::Window::create(*platform.value(),
                                           {.title = "vxe post test", .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto created = rhi::createGlDevice(*window.value(), {.debugOutput = true});
    REQUIRE(created.hasValue());
    rhi::Device& device = *created.value();
    {
        constexpr u32 kSize = 32;
        config::SettingsRegistry settings;
        auto post = render::PostProcess::create({.device = &device,
                                                 .settings = &settings,
                                                 .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
        REQUIRE(post.hasValue());
        const auto hdr =
            device.createTexture({.format = rhi::Format::RGBA16Float,
                                  .width = kSize,
                                  .height = kSize,
                                  .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled});
        const auto output =
            device.createTexture({.width = kSize,
                                  .height = kSize,
                                  .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
        REQUIRE(hdr.hasValue());
        REQUIRE(output.hasValue());
        // The "scene": every pixel 4.0, brighter than the screen can show.
        const auto render = [&]() {
            post.value()->update(kSize, kSize);
            rhi::CommandList commands;
            rhi::RenderPassDesc scene;
            scene.colorCount = 1;
            scene.colors[0] = {.texture = hdr.value(), .clearColor = {4.0F, 4.0F, 4.0F, 1.0F}};
            commands.beginRenderPass(scene);
            commands.endRenderPass();
            rhi::RenderPassDesc windowPass;
            windowPass.colorCount = 1;
            windowPass.colors[0] = {.texture = output.value(), .load = rhi::LoadOp::DontCare};
            commands.beginRenderPass(windowPass);
            post.value()->record(commands, hdr.value());
            commands.endRenderPass();
            device.beginFrame();
            device.submit(commands);
            device.present();
            std::vector<std::byte> pixels(std::size_t{kSize} * kSize * 4);
            REQUIRE(device.readTexture(output.value(), 0, 0, pixels).hasValue());
            return static_cast<int>(pixels[((std::size_t{16} * kSize) + 16) * 4]);
        };
        const auto expected = [](Tonemapper curve, f32 exposure) {
            return static_cast<int>(
                std::lround(render::encodeSrgb(render::tonemap(Vec3(4.0F), curve, exposure).x) * 255.0F));
        };
        const int aces = render();
        CHECK(aces == doctest::Approx(expected(Tonemapper::Aces, 1.0F)).epsilon(0.01));
        REQUIRE(settings.set("Graphics.Post.Tonemapper", static_cast<i32>(Tonemapper::Reinhard)).hasValue());
        const int reinhard = render(); // 4 / 5 = 0.8 linear, about 231 encoded.
        CHECK(reinhard == doctest::Approx(expected(Tonemapper::Reinhard, 1.0F)).epsilon(0.01));
        REQUIRE(settings.set("Graphics.Post.Tonemapper", static_cast<i32>(Tonemapper::None)).hasValue());
        CHECK(render() == 255); // Clipped.
        REQUIRE(settings.set("Graphics.Post.Exposure", 0.125F).hasValue());
        const int dimmed = render(); // 0.5 linear, about 188 encoded.
        CHECK(dimmed == doctest::Approx(expected(Tonemapper::None, 0.125F)).epsilon(0.01));
        MESSAGE("ACES " << aces << ", Reinhard " << reinhard << ", none at exposure 0.125 " << dimmed);
        CHECK(device.stats().apiErrors == 0);
        CHECK(device.stats().invalidCommands == 0);
        device.destroy(hdr.value());
        device.destroy(output.value());
    }
    device.waitIdle();
    CHECK(device.liveResources().empty());
}
