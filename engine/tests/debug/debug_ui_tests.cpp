#include "../test_helpers.hpp"

#include <vxe/debug/debug_ui.hpp>
#include <vxe/input/input.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/rhi_gl/gl_device.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>
#include <implot.h>

#include <array>
#include <memory>
#include <vector>

using namespace vxe;
using vxe::debug::DebugUi;

namespace {

std::unique_ptr<DebugUi> makeUi(rhi::Device& device, input::InputSystem& input) {
    auto ui = DebugUi::create(device, input, {.shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    REQUIRE(ui.hasValue());
    return std::move(ui.value());
}

// One full frame: begin, draw `content`, end, record into a pass over `pass`.
template <typename Content>
rhi::CommandList frame(DebugUi& ui, const rhi::RenderPassDesc& pass, Vec2 size, const Content& content) {
    ui.beginFrame(1.0 / 60.0, size, size);
    content();
    ui.endFrame();
    rhi::CommandList commands;
    commands.beginRenderPass(pass);
    ui.record(commands);
    commands.endRenderPass();
    return commands;
}

rhi::RenderPassDesc windowPass() {
    rhi::RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0] = {.swapchain = true, .load = rhi::LoadOp::Load};
    return pass;
}

} // namespace

TEST_CASE("Scancodes map to ImGui keys") {
    CHECK(debug::imguiKeyFromScancode(4) == ImGuiKey_A);
    CHECK(debug::imguiKeyFromScancode(29) == ImGuiKey_Z);
    CHECK(debug::imguiKeyFromScancode(30) == ImGuiKey_1);
    CHECK(debug::imguiKeyFromScancode(39) == ImGuiKey_0);
    CHECK(debug::imguiKeyFromScancode(40) == ImGuiKey_Enter);
    CHECK(debug::imguiKeyFromScancode(58) == ImGuiKey_F1);
    CHECK(debug::imguiKeyFromScancode(69) == ImGuiKey_F12);
    CHECK(debug::imguiKeyFromScancode(115) == ImGuiKey_F24);
    CHECK(debug::imguiKeyFromScancode(80) == ImGuiKey_LeftArrow);
    CHECK(debug::imguiKeyFromScancode(98) == ImGuiKey_Keypad0);
    CHECK(debug::imguiKeyFromScancode(97) == ImGuiKey_Keypad9);
    CHECK(debug::imguiKeyFromScancode(224) == ImGuiKey_LeftCtrl);
    CHECK(debug::imguiKeyFromScancode(231) == ImGuiKey_RightSuper);
    CHECK(debug::imguiKeyFromScancode(0) == ImGuiKey_None);
    CHECK(debug::imguiKeyFromScancode(300) == ImGuiKey_None);
}

TEST_CASE("The demo windows render through the Null device with valid commands only") {
    const auto device = rhi::createNullDevice();
    input::InputSystem input;
    {
        auto ui = makeUi(*device, input);
        for (int i = 0; i < 8; ++i) { // More frames than the buffer ring has sets.
            const rhi::CommandList commands = frame(*ui, windowPass(), {1280.0F, 720.0F}, [] {
                ImGui::ShowDemoWindow();
                ImPlot::ShowDemoWindow();
            });
            device->submit(commands);
            device->present();
        }
        const rhi::DeviceStats stats = device->stats();
        CHECK(stats.invalidCommands == 0);
        CHECK(stats.drawCalls > 0);
        CHECK(stats.textures >= 1); // The font atlas, created through ImGui's texture protocol.
    }
    device->waitIdle();
    CHECK(device->liveResources().empty()); // The UI cleans up after itself.
}

TEST_CASE("The debug UI takes input away from gameplay while ImGui uses it") {
    const auto device = rhi::createNullDevice();
    input::InputSystem input;
    const input::InputContextId gameplay = input.registerContext({.name = "Gameplay"});
    input.pushContext(gameplay);
    auto ui = makeUi(*device, input);

    const auto window = [] {
        ImGui::SetNextWindowPos({10.0F, 10.0F});
        ImGui::SetNextWindowSize({200.0F, 100.0F});
        ImGui::Begin("Hover me");
        ImGui::End();
    };
    const std::array<platform::PlatformEvent, 1> overWindow{
        platform::MouseMotionEvent{.x = 50.0F, .y = 50.0F}};
    const std::array<platform::PlatformEvent, 1> awayFromWindow{
        platform::MouseMotionEvent{.x = 900.0F, .y = 600.0F}};

    static_cast<void>(frame(*ui, windowPass(), {1280.0F, 720.0F}, window));
    CHECK_FALSE(ui->capturesInput());
    for (int i = 0; i < 2; ++i) { // Hover state follows the previous frame.
        ui->processEvents(overWindow);
        static_cast<void>(frame(*ui, windowPass(), {1280.0F, 720.0F}, window));
    }
    CHECK(ui->capturesInput());
    REQUIRE_FALSE(input.contextStack().empty());
    CHECK(input.contextStack().back() == ui->inputContext());

    for (int i = 0; i < 2; ++i) {
        ui->processEvents(awayFromWindow);
        static_cast<void>(frame(*ui, windowPass(), {1280.0F, 720.0F}, window));
    }
    CHECK_FALSE(ui->capturesInput());
    CHECK(input.contextStack().back() == gameplay);
}

TEST_CASE("Text fields ask for text input and receive typed text") {
    const auto device = rhi::createNullDevice();
    input::InputSystem input;
    auto ui = makeUi(*device, input);
    std::array<char, 32> buffer{};
    const auto window = [&buffer] {
        ImGui::SetNextWindowPos({0.0F, 0.0F});
        ImGui::Begin("Text");
        ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##field", buffer.data(), buffer.size());
        ImGui::End();
    };
    // Frame 1 asks for focus, frame 2 activates the field, frame 3 reports
    // that a text field wants input (ImGui reports state one frame later).
    for (int i = 0; i < 3; ++i) {
        static_cast<void>(frame(*ui, windowPass(), {800.0F, 600.0F}, window));
    }
    CHECK(ui->wantsTextInput());
    const std::array<platform::PlatformEvent, 1> typed{platform::TextInputEvent{.text = "vxe"}};
    ui->processEvents(typed);
    static_cast<void>(frame(*ui, windowPass(), {800.0F, 600.0F}, window));
    CHECK(std::string_view(buffer.data()) == "vxe");
}

TEST_CASE("ImGui draws through the OpenGL device without driver errors (VXE_GUI_TESTS=1)") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = platform::Platform::create();
    REQUIRE(platform.hasValue());
    auto window = platform::Window::create(*platform.value(),
                                           {.title = "vxe ImGui test", .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto created = rhi::createGlDevice(*window.value(), {.debugOutput = true});
    REQUIRE(created.hasValue());
    rhi::Device& device = *created.value();
    input::InputSystem input;
    {
        constexpr u32 kSize = 512;
        const rhi::TextureHandle target =
            device
                .createTexture({.format = rhi::Format::RGBA8Unorm,
                                .width = kSize,
                                .height = kSize,
                                .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc,
                                .debugName = "ImGui test target"})
                .value();
        rhi::RenderPassDesc pass;
        pass.colorCount = 1;
        pass.colors[0] = {.texture = target, .clearColor = {0.0F, 0.0F, 0.0F, 1.0F}};
        auto ui = makeUi(device, input);
        for (int i = 0; i < 4; ++i) {
            device.beginFrame();
            device.submit(frame(*ui, pass, {static_cast<f32>(kSize), static_cast<f32>(kSize)}, [] {
                // One window with a known place and size, so pixels can be
                // checked. (The demo window is covered by the Null device test
                // and the RHI demo; here ImGui would clamp it into the target.)
                ImGui::SetNextWindowPos({0.0F, 0.0F});
                ImGui::SetNextWindowSize({400.0F, 400.0F});
                ImGui::Begin("GPU test");
                ImGui::TextUnformatted("Drawn through the RHI");
                static_cast<void>(ImGui::Button("A button"));
                ImGui::End();
            }));
            device.present();
        }
        std::vector<std::byte> pixels(std::size_t{kSize} * kSize * 4);
        REQUIRE(device.readTexture(target, 0, 0, pixels).hasValue());
        std::size_t drawn = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i] != std::byte{0} || pixels[i + 1] != std::byte{0} || pixels[i + 2] != std::byte{0}) {
                ++drawn;
            }
        }
        MESSAGE("Pixels drawn by ImGui: " << drawn);
        CHECK(drawn > 400 * 400 * 9 / 10); // The window covers 400x400.
        // Inside the test window (rows count from the bottom, ADR-016): its
        // background is drawn. The lower right corner stays clear.
        const auto pixel = [&pixels](u32 x, u32 y) {
            const std::size_t index = (std::size_t{y} * kSize + x) * 4;
            return std::to_integer<int>(pixels[index]) + std::to_integer<int>(pixels[index + 1]) +
                   std::to_integer<int>(pixels[index + 2]);
        };
        CHECK(pixel(200, kSize - 300) > 0);
        CHECK(pixel(kSize - 5, 5) == 0);
        const rhi::DeviceStats stats = device.stats();
        CHECK(stats.invalidCommands == 0);
        CHECK(stats.apiErrors == 0);
        CHECK(stats.apiWarnings == 0);
        ui.reset();
        device.destroy(target);
    }
    device.waitIdle();
    CHECK(device.liveResources().empty());
    created.value().reset();
}
