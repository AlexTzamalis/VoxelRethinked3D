#include "vxe/render/camera.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace vxe::render {
namespace {

// SDL scancodes (USB HID usage ids) of the default keys.
constexpr u16 kKeyA = 4;
constexpr u16 kKeyD = 7;
constexpr u16 kKeyS = 22;
constexpr u16 kKeyW = 26;
constexpr u16 kKeySpace = 44;
constexpr u16 kKeyLeftCtrl = 224;
constexpr u16 kKeyLeftShift = 225;

// Just under 90 degrees: looking exactly straight up or down would make the
// forward vector parallel to the up vector, and the view matrix undefined.
constexpr f32 kMaxPitch = 1.5607964F;
constexpr f32 kWheelSpeedStep = 1.25F;

} // namespace

Vec3 Camera::forward() const noexcept {
    const f32 cosPitch = std::cos(pitch);
    return {std::sin(yaw) * cosPitch, std::sin(pitch), -std::cos(yaw) * cosPitch};
}

Vec3 Camera::right() const noexcept {
    return {std::cos(yaw), 0.0F, std::sin(yaw)};
}

Vec3 Camera::up() const noexcept {
    return glm::cross(right(), forward());
}

Mat4 Camera::view() const noexcept {
    return cameraRelativeView(forward());
}

Mat4 Camera::projection(f32 aspect) const noexcept {
    return perspectiveReversedInfinite(glm::radians(fovYDegrees), aspect, nearPlane);
}

FreeCameraController::FreeCameraController(const FreeCameraDesc& desc)
    : m_input(*desc.input)
    , m_settings(*desc.settings) {
    m_context = m_input.registerContext({.name = "Free camera"});
    m_input.pushContext(m_context);
    const auto action = [this](std::string_view id, std::string_view name, input::Binding binding) {
        return m_input.registerAction({.id = id,
                                       .displayName = name,
                                       .category = "Free camera",
                                       .context = m_context,
                                       .defaults = {binding}});
    };
    m_forward = action("debug.camera.forward", "Fly forward", input::Binding::key(kKeyW));
    m_back = action("debug.camera.back", "Fly back", input::Binding::key(kKeyS));
    m_left = action("debug.camera.left", "Fly left", input::Binding::key(kKeyA));
    m_right = action("debug.camera.right", "Fly right", input::Binding::key(kKeyD));
    m_up = action("debug.camera.up", "Fly up", input::Binding::key(kKeySpace));
    m_down = action("debug.camera.down", "Fly down", input::Binding::key(kKeyLeftShift));
    m_fast = action("debug.camera.fast", "Fly fast", input::Binding::key(kKeyLeftCtrl));
    m_look = action("debug.camera.look", "Look around (hold)",
                    input::Binding::mouse(platform::MouseButton::Right));
    m_faster =
        action("debug.camera.faster", "Fly speed up", input::Binding::wheel(input::BindingKind::WheelUp));
    m_slower =
        action("debug.camera.slower", "Fly speed down", input::Binding::wheel(input::BindingKind::WheelDown));

    using config::Visibility;
    m_speed = m_settings.registerFloat({{.id = "Debug.FreeCamera.Speed",
                                         .displayName = "Free camera speed",
                                         .description = "Blocks per second. The mouse wheel changes it too.",
                                         .visibility = Visibility::Developer},
                                        20.0F,
                                        0.5F,
                                        100000.0F,
                                        0.5F});
    m_fastMultiplier = m_settings.registerFloat({{.id = "Debug.FreeCamera.FastMultiplier",
                                                  .displayName = "Free camera fast multiplier",
                                                  .description = "Speed factor while the fast key is held.",
                                                  .visibility = Visibility::Developer},
                                                 10.0F,
                                                 1.0F,
                                                 100.0F,
                                                 0.5F});
    m_sensitivity =
        m_settings.registerFloat({{.id = "Debug.FreeCamera.Sensitivity",
                                   .displayName = "Free camera mouse sensitivity",
                                   .description = "Degrees of rotation per pixel of mouse motion.",
                                   .visibility = Visibility::Developer},
                                  0.15F,
                                  0.01F,
                                  2.0F,
                                  0.01F});
    m_fieldOfView = m_settings.registerFloat({{.id = "Debug.FreeCamera.FieldOfView",
                                               .displayName = "Free camera field of view",
                                               .description = "Vertical field of view in degrees.",
                                               .visibility = Visibility::Developer},
                                              70.0F,
                                              30.0F,
                                              120.0F,
                                              1.0F});
}

FreeCameraController::~FreeCameraController() {
    m_input.popContext(m_context);
}

bool FreeCameraController::isLooking() const noexcept {
    return m_input.isHeld(m_look);
}

void FreeCameraController::update(Camera& camera, f64 deltaSeconds) {
    camera.fovYDegrees = m_fieldOfView.get();
    if (m_input.isPressed(m_faster)) {
        static_cast<void>(m_speed.set(m_speed.get() * kWheelSpeedStep));
    }
    if (m_input.isPressed(m_slower)) {
        static_cast<void>(m_speed.set(m_speed.get() / kWheelSpeedStep));
    }
    if (isLooking()) {
        const Vec2 delta = m_input.mouseDelta();
        const f32 radiansPerPixel = glm::radians(m_sensitivity.get());
        camera.yaw = std::remainder(camera.yaw + delta.x * radiansPerPixel, glm::two_pi<f32>());
        camera.pitch = std::clamp(camera.pitch - delta.y * radiansPerPixel, -kMaxPitch, kMaxPitch);
    }

    // Moving on the horizontal plane, so looking down does not slow walking
    // forward; Space and Shift move straight up and down.
    const Vec3 flatForward = glm::normalize(Vec3(std::sin(camera.yaw), 0.0F, -std::cos(camera.yaw)));
    Vec3 direction{0.0F};
    const auto axis = [this](input::ActionId positive, input::ActionId negative) {
        return (m_input.isHeld(positive) ? 1.0F : 0.0F) - (m_input.isHeld(negative) ? 1.0F : 0.0F);
    };
    direction += flatForward * axis(m_forward, m_back);
    direction += camera.right() * axis(m_right, m_left);
    direction.y += axis(m_up, m_down);
    if (glm::dot(direction, direction) == 0.0F) {
        return;
    }
    f64 speed = static_cast<f64>(m_speed.get());
    if (m_input.isHeld(m_fast)) {
        speed *= static_cast<f64>(m_fastMultiplier.get());
    }
    camera.position += DVec3(glm::normalize(direction)) * (speed * deltaSeconds);
}

} // namespace vxe::render
