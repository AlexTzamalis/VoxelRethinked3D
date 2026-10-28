#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/debug/debug_ui.hpp>
#include <vxe/debug/overlay.hpp>
#include <vxe/input/input.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace vxe;
using debug::DebugOverlay;

namespace {

constexpr u16 kF3 = 60;
constexpr u16 kRight = 79;

// Everything an overlay needs, on the Null device.
struct Rig {
    std::unique_ptr<rhi::Device> device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    input::InputSystem input;
    stats::StatRegistry stats;
    std::unique_ptr<debug::DebugUi> ui;
    std::unique_ptr<DebugOverlay> overlay;

    explicit Rig(std::filesystem::path captures = "captures") {
        auto created = debug::DebugUi::create(*device, input,
                                              {.shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
        REQUIRE(created.hasValue());
        ui = std::move(created.value());
        overlay = std::make_unique<DebugOverlay>(debug::OverlayDesc{.settings = &settings,
                                                                    .input = &input,
                                                                    .stats = &stats,
                                                                    .device = device.get(),
                                                                    .captureFolder = std::move(captures)});
    }

    // One application frame, in the order DebugOverlay documents.
    void frame(std::span<const platform::PlatformEvent> events = {}, f64 seconds = 1.0 / 60.0) {
        input.update(events);
        ui->processEvents(events);
        overlay->processEvents(events);
        overlay->recordFrame(seconds * 1000.0, 0.5);
        stats.endFrame(seconds);
        ui->beginFrame(seconds, {1280.0F, 720.0F}, {1280.0F, 720.0F});
        overlay->update(seconds);
        ui->endFrame();
        rhi::CommandList commands;
        rhi::RenderPassDesc pass;
        pass.colorCount = 1;
        pass.colors[0] = {.swapchain = true, .load = rhi::LoadOp::Load};
        commands.beginRenderPass(pass);
        ui->record(commands);
        commands.endRenderPass();
        device->submit(commands);
        device->present();
        settings.dispatchChanges();
    }

    void key(u16 scancode, bool down) {
        const std::array<platform::PlatformEvent, 1> event{
            platform::KeyEvent{.scancode = scancode, .down = down}};
        frame(event);
    }
};

} // namespace

TEST_CASE("Frame time summaries give percentiles and lows") {
    std::vector<f64> times(100);
    std::iota(times.begin(), times.end(), 1.0); // 1 to 100 ms.
    std::vector<f64> scratch;
    const debug::FrameTimeSummary summary = debug::summarizeFrameTimes(times, scratch);
    CHECK(summary.averageMs == doctest::Approx(50.5));
    CHECK(summary.p50Ms == 50.0);
    CHECK(summary.p90Ms == 90.0);
    CHECK(summary.p99Ms == 99.0);
    CHECK(summary.p999Ms == 100.0);
    CHECK(summary.low1Fps == doctest::Approx(10.0)); // The slowest 1 percent: the 100 ms frame.
    CHECK(summary.averageFps == doctest::Approx(1000.0 / 50.5));
    CHECK(debug::summarizeFrameTimes({}, scratch).averageMs == 0.0);
}

TEST_CASE("F3 toggles the overlay on release, F3+Right switches pages without toggling") {
    Rig rig;
    CHECK_FALSE(rig.overlay->isVisible());
    rig.key(kF3, true);
    CHECK_FALSE(rig.overlay->isVisible()); // Not yet: F3 might start a chord.
    rig.key(kF3, false);
    CHECK(rig.overlay->isVisible());
    CHECK(rig.overlay->currentPage() == 0);

    rig.key(kF3, true);
    rig.key(kRight, true);
    CHECK(rig.overlay->currentPage() == 1);
    CHECK(rig.overlay->pageName(1) == "Frame");
    rig.key(kRight, false);
    rig.key(kF3, false);
    CHECK(rig.overlay->isVisible()); // The chord used F3, so its release did not hide the overlay.

    rig.key(kF3, true);
    rig.key(kF3, false);
    CHECK_FALSE(rig.overlay->isVisible());
    // The overlay's state lives in settings, so it is saved and live editable.
    CHECK(rig.settings.value("Debug.Overlay.Page").value() == config::SettingValue{1});
}

TEST_CASE("Every overlay page draws with valid commands only") {
    Rig rig;
    rig.overlay->addPage("Custom", [] {
        ImGui::Begin("Custom");
        ImGui::TextUnformatted("A page added by a system");
        ImGui::End();
    });
    rig.overlay->addOverviewLine([] { return std::string("Chunk: 1 2 3"); });
    rig.overlay->setVisible(true);
    static_cast<void>(rig.settings.set("Debug.Overlay.Opacity", config::SettingValue{0.5F}));
    for (usize page = 0; page < rig.overlay->pageCount(); ++page) {
        rig.overlay->showPage(page);
        for (int i = 0; i < 3; ++i) {
            rig.frame();
        }
    }
    CHECK(rig.overlay->pageCount() == 8);
    const rhi::DeviceStats stats = rig.device->stats();
    CHECK(stats.invalidCommands == 0);
    CHECK(stats.drawCalls > 0);
    // The Settings diff page lists exactly what was changed.
    const auto changed = debug::changedSettings(rig.settings);
    const auto has = [&changed](std::string_view id) {
        return std::ranges::any_of(changed, [id](const config::SettingInfo* info) { return info->id == id; });
    };
    CHECK(has("Debug.Overlay.Opacity"));
    CHECK(has("Debug.Overlay.Visible"));
    CHECK_FALSE(has("Debug.Overlay.Scale"));
}

TEST_CASE("Frame and GPU stats reach the registry") {
    Rig rig;
    for (int i = 0; i < 10; ++i) {
        rig.frame();
    }
    const stats::StatId cpu = rig.stats.find("Frame.CpuTime");
    REQUIRE(cpu.isValid());
    CHECK(rig.stats.value(cpu) == doctest::Approx(1000.0 / 60.0));
    CHECK(rig.stats.value(rig.stats.find("Frame.GpuTime")) == 0.5);
    CHECK(rig.stats.value(rig.stats.find("Gpu.DrawCalls")) >= 0.0);
    CHECK(rig.stats.find("Memory.Allocations").isValid());
    CHECK(rig.stats.find("Debug.Overlay.DrawTime").isValid());
}

TEST_CASE("A CSV capture records every stat for the configured time") {
    static std::atomic<int> run{0};
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / std::format("vxe_overlay_csv_{}", run.fetch_add(1));
    std::filesystem::remove_all(folder);
    {
        Rig rig(folder);
        static_cast<void>(rig.settings.set("Debug.Overlay.CsvSeconds", config::SettingValue{1}));
        rig.overlay->startCsvCapture();
        CHECK(rig.overlay->csv().isRecording());
        for (int i = 0; i < 4; ++i) { // 4 frames of 0.25 s fill the 1 s capture.
            rig.frame({}, 0.25);
        }
        CHECK_FALSE(rig.overlay->csv().isRecording());
        rig.overlay->csv().waitForWrite();
        CHECK(rig.overlay->csv().lastResult().starts_with("Saved "));
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        files.push_back(entry.path());
    }
    REQUIRE(files.size() == 1);
    std::ifstream file(files.front());
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);) {
        lines.push_back(line);
    }
    REQUIRE(lines.size() == 5); // Header plus 4 frames.
    CHECK(lines[0].starts_with("frame,seconds,"));
    CHECK(lines[0].find("Frame.CpuTime") != std::string::npos);
    CHECK(lines[4].starts_with("3,1.000000,"));
    file.close();
    std::filesystem::remove_all(folder);
}

TEST_CASE("The overlay stays within its frame budget") {
    Rig rig;
    for (int i = 0; i < 30; ++i) { // Warm up ImGui's font atlas and window state.
        rig.frame();
    }
    const auto measure = [&rig](int frames) {
        Duration total{};
        for (int i = 0; i < frames; ++i) {
            rig.input.update({});
            rig.overlay->recordFrame(16.0, 1.0);
            rig.stats.endFrame(1.0 / 60.0);
            rig.ui->beginFrame(1.0 / 60.0, {1280.0F, 720.0F}, {1280.0F, 720.0F});
            const TimePoint start = Clock::now();
            rig.overlay->update(1.0 / 60.0);
            total += Clock::now() - start;
            rig.ui->endFrame();
        }
        return toMilliseconds(total) / frames;
    };
    const f64 hidden = measure(200);
    rig.overlay->setVisible(true);
    rig.settings.dispatchChanges();
    const f64 compact = measure(200);
    MESSAGE("Overlay update: " << hidden << " ms hidden, " << compact << " ms compact");
#if defined(NDEBUG) && !defined(VXE_SANITIZER_ACTIVE)
    // Section 18.3 budgets on real machines; ten times the room on shared CI
    // machines, which stall threads at random.
    const f64 room = test::timingIsReliable() ? 1.0 : 10.0;
    CHECK(hidden < 0.02 * room); // "0 ms when hidden".
    CHECK(compact < 0.2 * room); // Compact mode under 0.2 ms.
#endif
}
