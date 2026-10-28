#include <vxe/config/console.hpp>
#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/debug/debug_ui.hpp>
#include <vxe/debug/developer_console.hpp>
#include <vxe/input/input.hpp>
#include <vxe/platform/frame_limiter.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <string>

using namespace vxe;
using debug::DeveloperConsole;

namespace {

constexpr u16 kGrave = 53;

// A console over the settings commands, with the Video.FpsLimit setting a
// frame loop registers.
struct ConsoleRig {
    config::SettingsRegistry settings;
    config::CommandRegistry commands;
    input::InputSystem input;
    stats::StatRegistry stats;
    config::Setting<i32> fpsLimit;
    std::unique_ptr<DeveloperConsole> console;

    ConsoleRig() {
        fpsLimit = settings.registerInt({{.id = "Video.FpsLimit",
                                          .displayName = "FPS limit",
                                          .description = "Frames per second cap, 0 for none."},
                                         0,
                                         0,
                                         1000,
                                         1});
        config::registerSettingsCommands(commands, settings);
        console = std::make_unique<DeveloperConsole>(debug::ConsoleDesc{
            .commands = &commands, .settings = &settings, .input = &input, .stats = &stats});
    }
};

} // namespace

TEST_CASE("Console lines run commands and print what they print") {
    ConsoleRig rig;
    CHECK(rig.console->execute("set Video.FpsLimit 144"));
    CHECK(rig.fpsLimit.get() == 144);
    const auto& lines = rig.console->lines();
    REQUIRE(lines.size() >= 2);
    CHECK(lines[lines.size() - 2].text == "> set Video.FpsLimit 144");
    CHECK(lines[lines.size() - 2].kind == DeveloperConsole::LineKind::Input);

    CHECK_FALSE(rig.console->execute("set Video.FpsLimit lots"));
    CHECK(rig.console->lines().back().kind == DeveloperConsole::LineKind::Error);
    CHECK(rig.fpsLimit.get() == 144);

    CHECK(rig.console->execute("   ")); // Blank lines do nothing.
    rig.stats.endFrame(0.016);
    CHECK(rig.stats.value(rig.stats.find("Debug.Console.Commands")) == 2.0);
}

TEST_CASE("Console history walks back and forward") {
    const ConsoleRig rig;
    CHECK_FALSE(rig.console->historyUp().has_value()); // Nothing typed yet.
    static_cast<void>(rig.console->execute("get Video.FpsLimit"));
    static_cast<void>(rig.console->execute("help"));
    static_cast<void>(rig.console->execute("help")); // Repeats are kept once.
    CHECK(rig.console->history() == std::vector<std::string>{"get Video.FpsLimit", "help"});
    CHECK(rig.console->historyUp() == "help");
    CHECK(rig.console->historyUp() == "get Video.FpsLimit");
    CHECK(rig.console->historyUp() == "get Video.FpsLimit"); // Stops at the oldest.
    CHECK(rig.console->historyDown() == "help");
    CHECK(rig.console->historyDown() == ""); // Past the newest: an empty line.
    CHECK_FALSE(rig.console->historyDown().has_value());
}

TEST_CASE("Tab completes the word being typed") {
    const ConsoleRig rig;
    CHECK(rig.console->completeLine("se") == "set ");
    CHECK(rig.console->completeLine("set video.fps") == "set Video.FpsLimit ");
    // Several candidates: the line grows to their common start, and they are listed.
    const usize before = rig.console->lines().size();
    CHECK(rig.console->completeLine("set Debug.Console.") == "set Debug.Console.");
    CHECK(rig.console->lines().size() == before + 1);
    CHECK(rig.console->lines().back().text.find("Debug.Console.MaxLines") != std::string::npos);
    CHECK(rig.console->completeLine("set Debug.C") == "set Debug.Console.");
    CHECK(rig.console->completeLine("nothing matches") == "nothing matches");
}

TEST_CASE("The Grave key opens the console, which blocks gameplay input") {
    ConsoleRig rig;
    const input::InputContextId gameplay = rig.input.registerContext({.name = "Gameplay"});
    rig.input.pushContext(gameplay);
    const auto press = [&rig](bool down) {
        const std::array<platform::PlatformEvent, 1> event{
            platform::KeyEvent{.scancode = kGrave, .down = down}};
        rig.input.update(event);
        rig.console->handleInput();
    };
    press(true);
    CHECK(rig.console->isOpen());
    CHECK(rig.input.contextName(rig.input.contextStack().back()) == "Console");
    press(false);
    press(true);
    CHECK_FALSE(rig.console->isOpen());
    CHECK(rig.input.contextStack().back() == gameplay);
}

TEST_CASE("The console window draws with valid commands only") {
    ConsoleRig rig;
    const auto device = rhi::createNullDevice();
    auto ui = debug::DebugUi::create(*device, rig.input,
                                     {.shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    REQUIRE(ui.hasValue());
    rig.console->setOpen(true);
    static_cast<void>(rig.console->execute("help"));
    for (int i = 0; i < 4; ++i) {
        ui.value()->beginFrame(1.0 / 60.0, {1280.0F, 720.0F}, {1280.0F, 720.0F});
        rig.console->update();
        ui.value()->endFrame();
        rhi::CommandList commands;
        rhi::RenderPassDesc pass;
        pass.colorCount = 1;
        pass.colors[0] = {.swapchain = true, .load = rhi::LoadOp::Load};
        commands.beginRenderPass(pass);
        ui.value()->record(commands);
        commands.endRenderPass();
        device->submit(commands);
    }
    CHECK(device->stats().invalidCommands == 0);
    CHECK(device->stats().drawCalls > 0);
    CHECK(ui.value()->wantsTextInput()); // The input field has focus.
}

TEST_CASE("The frame limiter holds the requested rate") {
    auto platform = platform::Platform::create(platform::PlatformConfig{.video = false});
    REQUIRE(platform.hasValue());
    platform::FrameLimiter limiter;
    constexpr int kFrames = 100;
    limiter.wait(200.0); // Starts the schedule.
    const TimePoint start = Clock::now();
    for (int i = 0; i < kFrames; ++i) {
        limiter.wait(200.0);
    }
    const f64 fps = kFrames / toSeconds(Clock::now() - start);
    MESSAGE("Limited to 200 FPS, measured " << fps);
    CHECK(fps == doctest::Approx(200.0).epsilon(0.05));
    // 0 means no limit.
    const TimePoint unlimited = Clock::now();
    for (int i = 0; i < kFrames; ++i) {
        limiter.wait(0.0);
    }
    CHECK(toMilliseconds(Clock::now() - unlimited) < 5.0);
}

// E7.4 acceptance: "set Video.FpsLimit 144" typed into the console changes the
// frame rate of a running loop on its next frame.
TEST_CASE("set Video.FpsLimit 144 takes effect live") {
    auto platform = platform::Platform::create(platform::PlatformConfig{.video = false});
    REQUIRE(platform.hasValue());
    ConsoleRig rig;
    platform::FrameLimiter limiter;
    const auto measure = [&rig, &limiter](int frames) {
        limiter.wait(static_cast<f64>(rig.fpsLimit.get()));
        const TimePoint start = Clock::now();
        for (int i = 0; i < frames; ++i) {
            limiter.wait(static_cast<f64>(rig.fpsLimit.get())); // Read every frame, as live settings must be.
        }
        return frames / toSeconds(Clock::now() - start);
    };
    static_cast<void>(rig.console->execute("set Video.FpsLimit 500"));
    const f64 before = measure(100);
    REQUIRE(rig.console->execute("set Video.FpsLimit 144"));
    const f64 after = measure(72);
    MESSAGE("FPS at limit 500: " << before << ", after set Video.FpsLimit 144: " << after);
    CHECK(after == doctest::Approx(144.0).epsilon(0.05));
    CHECK(before > 300.0);
}
