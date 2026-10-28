#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/render/frame_pacer.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <array>
#include <thread>

using namespace vxe;

TEST_CASE("The lowest frame rate cap that applies wins") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    render::FramePacer pacer({.device = device.get(), .settings = &settings});
    CHECK(pacer.limitFor(true, false) == 0.0);   // Defaults: no cap in game,
    CHECK(pacer.limitFor(false, false) == 30.0); // 30 in the background,
    CHECK(pacer.limitFor(true, true) == 120.0);  // 120 in menus,
    CHECK(pacer.limitFor(false, true) == 30.0);  // the lower of the two.
    REQUIRE(settings.set("Video.FpsLimit", 60).hasValue());
    CHECK(pacer.limitFor(true, false) == 60.0);
    CHECK(pacer.limitFor(true, true) == 60.0); // 60 is below the menu cap.
    REQUIRE(settings.set("Video.FpsLimitUnfocused", 0).hasValue());
    CHECK(pacer.limitFor(false, false) == 60.0); // 0 turns that cap off.
    pacer.update();                              // Applies the present mode; must not fail on any device.
    CHECK(settings.find("Video.PresentMode") != nullptr);
}

// E8.4 acceptance: the frame limiter holds its rate within 2%, also when the
// frames themselves take a varying amount of work.
TEST_CASE("The frame limiter is accurate within 2 percent") {
    auto platform = platform::Platform::create(platform::PlatformConfig{.video = false});
    REQUIRE(platform.hasValue());
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    render::FramePacer pacer({.device = device.get(), .settings = &settings, .stats = &stats});
    for (const i32 target : std::array<i32, 3>{60, 144, 240}) {
        REQUIRE(settings.set("Video.FpsLimit", target).hasValue());
        const i32 frames = target / 2; // Half a second each.
        pacer.wait(true, false);       // Starts the schedule.
        const TimePoint start = Clock::now();
        for (i32 i = 0; i < frames; ++i) {
            // Work of 0 to 2 ms, varying every frame, like a real frame would.
            std::this_thread::sleep_for(std::chrono::microseconds((i * 7919) % 2000));
            pacer.wait(true, false);
        }
        const f64 fps = static_cast<f64>(frames) / toSeconds(Clock::now() - start);
        MESSAGE("Limit " << target << " FPS: measured " << fps);
        CHECK(fps < static_cast<f64>(target) * 1.02); // Never faster than the cap.
        if (test::timingIsReliable()) {
            CHECK(fps == doctest::Approx(static_cast<f64>(target)).epsilon(0.02));
        }
    }
    stats.endFrame(1.0 / 60.0);
    CHECK(stats.find("Frame.LimiterWait").isValid());
}
