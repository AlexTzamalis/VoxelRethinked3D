#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>
#include <vxe/math/projection.hpp>
#include <vxe/math/vector.hpp>

namespace vxe::render {

// A perspective camera with a double precision world position (ADR-011).
//
// Nothing the GPU sees is ever relative to the world origin: matrices are
// built for camera relative positions (view() only rotates), and positions
// are made camera relative before they become floats, either on the CPU in
// double (relative()) or on the GPU from integer chunk offsets and the
// camera's chunk and fraction (renderPosition()). That is what keeps the
// picture steady millions of blocks from the origin.
//
// Angles: yaw 0 looks along -Z and positive yaw turns right (towards +X);
// pitch is up positive, limited to just under straight up or down.
struct Camera {
    DVec3 position{0.0};
    f32 yaw = 0.0F;   // Radians.
    f32 pitch = 0.0F; // Radians.
    f32 fovYDegrees = 70.0F;
    f32 nearPlane = 0.05F; // Blocks. The far plane is infinite (ADR-010).

    [[nodiscard]] Vec3 forward() const noexcept;
    [[nodiscard]] Vec3 right() const noexcept;
    [[nodiscard]] Vec3 up() const noexcept;
    // Rotation only: maps camera relative positions to view space.
    [[nodiscard]] Mat4 view() const noexcept;
    [[nodiscard]] Mat4 projection(f32 aspect) const noexcept;
    [[nodiscard]] Mat4 viewProjection(f32 aspect) const noexcept { return projection(aspect) * view(); }
    // The camera's chunk and the position inside it, for shaders that place
    // chunk geometry with integer offsets (Section 8.4).
    [[nodiscard]] CameraPosition renderPosition() const noexcept {
        return CameraPosition::fromWorld(position);
    }
    // A world position relative to the camera. The subtraction is done in
    // double, so the result is exact for anything near the camera.
    [[nodiscard]] Vec3 relative(const DVec3& world) const noexcept { return Vec3(world - position); }
};

struct FreeCameraDesc {
    input::InputSystem* input = nullptr;
    config::SettingsRegistry* settings = nullptr;
};

// Flies a Camera with the keyboard and mouse, for debugging and test scenes:
// W, A, S, D move, Space and Left Shift go up and down, Left Ctrl is fast,
// the mouse looks while the right button is held and the wheel changes the
// speed. Registers its own input context "Free camera" (not blocking) and
// the settings Debug.FreeCamera.* (speed, fast multiplier, sensitivity,
// field of view), all live.
//
// Thread safety: main thread only.
class FreeCameraController {
public:
    explicit FreeCameraController(const FreeCameraDesc& desc);
    ~FreeCameraController();
    FreeCameraController(const FreeCameraController&) = delete;
    FreeCameraController& operator=(const FreeCameraController&) = delete;
    FreeCameraController(FreeCameraController&&) = delete;
    FreeCameraController& operator=(FreeCameraController&&) = delete;

    // Applies this frame's input. Call after InputSystem::update.
    void update(Camera& camera, f64 deltaSeconds);
    // True while the mouse is steering the camera (right button held), so
    // the program can hide the cursor and lock it to the window.
    [[nodiscard]] bool isLooking() const noexcept;
    [[nodiscard]] input::InputContextId inputContext() const noexcept { return m_context; }

private:
    input::InputSystem& m_input;
    config::SettingsRegistry& m_settings;
    input::InputContextId m_context;
    input::ActionId m_forward;
    input::ActionId m_back;
    input::ActionId m_left;
    input::ActionId m_right;
    input::ActionId m_up;
    input::ActionId m_down;
    input::ActionId m_fast;
    input::ActionId m_look;
    input::ActionId m_faster;
    input::ActionId m_slower;
    config::Setting<f32> m_speed;
    config::Setting<f32> m_fastMultiplier;
    config::Setting<f32> m_sensitivity;
    config::Setting<f32> m_fieldOfView;
};

} // namespace vxe::render
