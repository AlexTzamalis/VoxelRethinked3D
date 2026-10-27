#include "../test_helpers.hpp"

#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <variant>
#include <vector>

using vxe::usize;
using vxe::platform::KeyEvent;
using vxe::platform::Modifiers;
using vxe::platform::MouseButton;
using vxe::platform::MouseButtonEvent;
using vxe::platform::MouseMotionEvent;
using vxe::platform::MouseWheelEvent;
using vxe::platform::Platform;
using vxe::platform::PlatformConfig;
using vxe::platform::PlatformEvent;
using vxe::platform::QuitRequested;
using vxe::platform::Window;
using vxe::platform::WindowDesc;
using vxe::platform::WindowMode;

namespace {

void push(SDL_Event event) {
    REQUIRE(SDL_PushEvent(&event));
}

} // namespace

TEST_CASE("The platform starts headless and refuses a second instance") {
    auto platform = Platform::create(PlatformConfig{.video = false});
    REQUIRE(platform.hasValue());
    CHECK_FALSE(platform.value()->hasVideo());

    const auto second = Platform::create(PlatformConfig{.video = false});
    CHECK(second.hasError());

    CHECK_FALSE(Platform::executableFolder().empty());
    CHECK(Window::create(*platform.value(), WindowDesc{}).hasError()); // No video, no window.
}

TEST_CASE("SDL events are translated into engine events") {
    auto platform = Platform::create(PlatformConfig{.video = false});
    REQUIRE(platform.hasValue());

    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_W;
    key.key.mod = SDL_KMOD_LSHIFT;
    key.key.repeat = false;
    push(key);

    SDL_Event button{};
    button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    button.button.button = SDL_BUTTON_RIGHT;
    button.button.clicks = 2;
    button.button.x = 10.0F;
    button.button.y = 20.0F;
    push(button);

    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.xrel = 3.5F;
    motion.motion.yrel = -1.0F;
    push(motion);

    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1.0F;
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED; // Normalized back to "away from the user".
    push(wheel);

    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    push(quit);

    std::vector<PlatformEvent> events;
    CHECK(platform.value()->pollEvents(events) >= 5);

    // Other events (for example from the OS) may be mixed in; look for ours.
    bool sawKey = false;
    bool sawButton = false;
    bool sawMotion = false;
    bool sawWheel = false;
    bool sawQuit = false;
    for (const PlatformEvent& event : events) {
        if (const auto* k = std::get_if<KeyEvent>(&event)) {
            sawKey = k->scancode == SDL_SCANCODE_W && k->down && !k->repeat &&
                     hasAll(k->modifiers, Modifiers::Shift);
        } else if (const auto* b = std::get_if<MouseButtonEvent>(&event)) {
            sawButton = b->button == MouseButton::Right && b->down && b->clicks == 2 && b->x == 10.0F;
        } else if (const auto* m = std::get_if<MouseMotionEvent>(&event)) {
            sawMotion = m->dx == 3.5F && m->dy == -1.0F;
        } else if (const auto* w = std::get_if<MouseWheelEvent>(&event)) {
            sawWheel = w->y == -1.0F;
        } else if (std::holds_alternative<QuitRequested>(event)) {
            sawQuit = true;
        }
    }
    CHECK(sawKey);
    CHECK(sawButton);
    CHECK(sawMotion);
    CHECK(sawWheel);
    CHECK(sawQuit);
}

TEST_CASE("Window opens, resizes, toggles fullscreen and closes (VXE_GUI_TESTS=1)") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = Platform::create();
    REQUIRE(platform.hasValue());
    std::vector<PlatformEvent> events;
    const auto pump = [&] {
        for (int i = 0; i < 20; ++i) {
            static_cast<void>(platform.value()->pollEvents(events));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    auto window = Window::create(
        *platform.value(),
        WindowDesc{.title = "vxe window test", .width = 800, .height = 450, .debugContext = true});
    REQUIRE(window.hasValue());
    Window& w = *window.value();
    CHECK(w.glContext() != nullptr);
    CHECK(w.glContextInfo().major == 4);
    CHECK(w.glContextInfo().minor >= 5);
    CHECK(Window::glGetProcAddress("glGetString") != nullptr);
    pump();
    CHECK(w.size() == vxe::IVec2(800, 450));
    CHECK(w.pixelSize().x >= 800); // Larger on high DPI screens.

    w.setSize(1024, 576);
    pump();
    CHECK(w.size() == vxe::IVec2(1024, 576));

    REQUIRE(w.setMode(WindowMode::Borderless).hasValue());
    pump();
    CHECK(w.mode() == WindowMode::Borderless);
    REQUIRE(w.setMode(WindowMode::Fullscreen).hasValue());
    pump();
    CHECK(w.mode() == WindowMode::Fullscreen);
    REQUIRE(w.setMode(WindowMode::Windowed).hasValue());
    pump();
    CHECK(w.mode() == WindowMode::Windowed);

    REQUIRE(w.setRelativeMouseMode(true).hasValue());
    CHECK(w.isRelativeMouseMode());
    REQUIRE(w.setRelativeMouseMode(false).hasValue());
    CHECK(w.setGlSwapInterval(1));
    w.swapGlBuffers();

    window.value().reset(); // Closes the window before the platform shuts down.
    pump();
}
