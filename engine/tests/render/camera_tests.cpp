#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/input/input.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/frame_constants.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>
#include <glm/trigonometric.hpp>

#include <array>
#include <cstring>
#include <variant>
#include <vector>

using namespace vxe;
using render::Camera;

namespace {

bool near(const Vec3& a, const Vec3& b) {
    return glm::length(a - b) < 1e-5F;
}

} // namespace

TEST_CASE("Camera angles give the documented directions") {
    Camera camera;
    CHECK(near(camera.forward(), {0.0F, 0.0F, -1.0F}));
    CHECK(near(camera.right(), {1.0F, 0.0F, 0.0F}));
    CHECK(near(camera.up(), {0.0F, 1.0F, 0.0F}));
    camera.yaw = glm::radians(90.0F); // Turned right: looking along +X.
    CHECK(near(camera.forward(), {1.0F, 0.0F, 0.0F}));
    CHECK(near(camera.right(), {0.0F, 0.0F, 1.0F}));
    camera.yaw = 0.0F;
    camera.pitch = glm::radians(30.0F);
    CHECK(camera.forward().y == doctest::Approx(0.5F));
}

TEST_CASE("The view projection is camera relative with reversed depth") {
    Camera camera;
    camera.position = {1.0e7, 64.0, -3.0e6}; // Far from the origin: must not matter.
    const Mat4 viewProjection = camera.viewProjection(16.0F / 9.0F);
    // A point 10 blocks ahead: centered, depth near / distance (reversed Z).
    const Vec4 ahead = viewProjection * Vec4(camera.relative(camera.position + DVec3(0.0, 0.0, -10.0)), 1.0F);
    CHECK(ahead.x / ahead.w == doctest::Approx(0.0F));
    CHECK(ahead.y / ahead.w == doctest::Approx(0.0F));
    CHECK(ahead.z / ahead.w == doctest::Approx(camera.nearPlane / 10.0F));
    // A point behind the camera has negative w and is clipped.
    const Vec4 behind = viewProjection * Vec4(0.0F, 0.0F, 10.0F, 1.0F);
    CHECK(behind.w < 0.0F);
}

TEST_CASE("Frame constants split the camera into chunk and fraction") {
    Camera camera;
    camera.position = {1.0e7 + 3.25, -20.5, 1.0e7 + 15.75};
    const render::FrameConstants constants = render::makeFrameConstants(
        camera, {.width = 1920, .height = 1080, .seconds = 3600.5, .deltaSeconds = 0.004, .frameIndex = 42});
    // 10,000,000 / 16 = 625,000 exactly, so the fractions are the small parts.
    CHECK(constants.cameraChunk.x == 625000);
    CHECK(constants.cameraChunk.y == -2); // -20.5 lies in chunk -2 (blocks -32 to -17).
    CHECK(constants.cameraChunk.z == 625000);
    CHECK(constants.cameraFraction.x == 3.25F);
    CHECK(constants.cameraFraction.y == 11.5F);
    CHECK(constants.cameraFraction.z == 15.75F);
    CHECK(constants.viewport.x == 1920.0F);
    CHECK(constants.viewport.w == doctest::Approx(1.0F / 1080.0F));
    CHECK(constants.time.x == doctest::Approx(0.5F)); // Wrapped hourly.
    CHECK(constants.time.z == 42.0F);
    const Mat4 identity = constants.viewProjection * constants.inverseViewProjection;
    CHECK(identity[0][0] == doctest::Approx(1.0F));
    CHECK(identity[3][2] == doctest::Approx(0.0F).epsilon(1e-4));
}

TEST_CASE("The frame constant ring hands out a different buffer each frame") {
    const auto device = rhi::createNullDevice();
    render::FrameConstantsBuffer buffer;
    REQUIRE(buffer.create(*device).hasValue());
    std::vector<rhi::BufferHandle> used;
    for (int i = 0; i < 4; ++i) {
        render::FrameConstants constants;
        constants.time.z = static_cast<f32>(i);
        buffer.update(*device, constants);
        rhi::CommandList commands;
        buffer.bind(commands);
        const rhi::CommandView bind = *commands.begin();
        REQUIRE(bind.type == rhi::CommandType::BindUniformBuffer);
        used.push_back(bind.as<rhi::cmd::BindBufferRange>().buffer);
        device->submit(commands);
    }
    CHECK(used[0] != used[1]);
    CHECK(used[1] != used[2]);
    CHECK(used[3] == used[0]); // Three buffers, round robin.
    buffer.destroy(*device);
    CHECK(device->liveResources().empty());
}

TEST_CASE("The free camera flies and looks with its actions") {
    config::SettingsRegistry settings;
    input::InputSystem input;
    render::FreeCameraController controller({.input = &input, .settings = &settings});
    Camera camera;
    const auto frame = [&](std::vector<platform::PlatformEvent> events, f64 seconds) {
        input.update(events);
        controller.update(camera, seconds);
    };
    // W held for half a second at the default 20 blocks per second.
    frame({platform::KeyEvent{.scancode = 26, .down = true}}, 0.5);
    CHECK(camera.position.z == doctest::Approx(-10.0));
    frame({platform::KeyEvent{.scancode = 26, .down = false}}, 0.5);
    CHECK(camera.position.z == doctest::Approx(-10.0));

    // Mouse motion only turns the camera while the right button is held.
    frame({platform::MouseMotionEvent{.dx = 100.0F}}, 0.016);
    CHECK(camera.yaw == 0.0F);
    CHECK_FALSE(controller.isLooking());
    frame({platform::MouseButtonEvent{.button = platform::MouseButton::Right, .down = true},
           platform::MouseMotionEvent{.dx = 100.0F, .dy = -50.0F}},
          0.016);
    CHECK(controller.isLooking());
    CHECK(camera.yaw == doctest::Approx(glm::radians(15.0F)));
    CHECK(camera.pitch == doctest::Approx(glm::radians(7.5F)));

    // The wheel speeds up, Ctrl multiplies, Space goes straight up.
    frame({platform::MouseWheelEvent{.y = 1.0F}}, 0.0);
    CHECK(std::get<f32>(settings.value("Debug.FreeCamera.Speed").value()) == doctest::Approx(25.0F));
    const f64 startY = camera.position.y;
    frame(
        {platform::KeyEvent{.scancode = 44, .down = true}, platform::KeyEvent{.scancode = 224, .down = true}},
        0.1);
    CHECK(camera.position.y - startY == doctest::Approx(25.0 * 10.0 * 0.1));
}
