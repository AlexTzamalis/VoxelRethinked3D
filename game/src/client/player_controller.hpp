#pragma once

#include "app/game_settings.hpp"
#include "common/block_catalog.hpp"
#include "common/player_physics.hpp"
#include "server/world_store.hpp"

#include <vxe/input/input.hpp>
#include <vxe/math/raycast.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <array>
#include <functional>
#include <optional>

namespace vr {

// The blocks the player carries (G6.4): nine slots of block states, kAirState
// for an empty slot.
struct Hotbar {
    std::array<vxe::voxel::BlockStateId, 9> slots{};
    vxe::usize selected = 0;

    [[nodiscard]] vxe::voxel::BlockStateId current() const { return slots.at(selected); }
};

// Turns input into a walking or flying player (Section 20.4, G6.1): mouse
// look with the captured cursor, movement through stepPlayer at its fixed
// step, the camera at the eyes between two steps so it stays smooth at any
// frame rate, double Space to fly, zoom on a key with the wheel, view bobbing.
//
// Thread safety: main thread only.
class PlayerController {
public:
    PlayerController(vxe::input::InputSystem& input, const GameActions& actions, const GameSettings& settings,
                     const BlockCatalog& catalog)
        : m_input(input)
        , m_actions(actions)
        , m_settings(settings)
        , m_catalog(catalog) {}

    // Starts over at `feet`, standing still.
    void place(const vxe::DVec3& feet, vxe::f32 yaw, vxe::f32 pitch, bool flying);
    // Puts the camera where the eyes are, for frames without update().
    void applyTo(vxe::render::Camera& camera) const;
    // One frame of play. `bottom` and `top` are the world's height in blocks.
    // Without `look` the mouse belongs to a screen, and the world still runs.
    void update(vxe::render::Camera& camera, const vxe::voxel::VoxelWorld& world, vxe::f64 deltaSeconds,
                vxe::i64 bottom, vxe::i64 top, bool look = true);
    // The field of view with the zoom applied.
    [[nodiscard]] vxe::f32 fieldOfView(vxe::f32 degrees) const noexcept { return degrees / m_zoom; }
    // The wheel zooms while this is true, so it must not also change slots.
    [[nodiscard]] bool isZooming() const;
    [[nodiscard]] const PlayerState& state() const noexcept { return m_state; }
    [[nodiscard]] vxe::f32 yaw() const noexcept { return m_yaw; }
    [[nodiscard]] vxe::f32 pitch() const noexcept { return m_pitch; }

private:
    vxe::input::InputSystem& m_input;
    const GameActions& m_actions;
    const GameSettings& m_settings;
    const BlockCatalog& m_catalog;
    PlayerState m_state;
    vxe::DVec3 m_previous{}; // Position before the last step.
    vxe::f64 m_accumulator = 0.0;
    vxe::f32 m_yaw = 0.0F;
    vxe::f32 m_pitch = 0.0F;
    vxe::f32 m_zoom = 1.0F;       // Field of view divisor, eased.
    vxe::f32 m_zoomTarget = 4.0F; // While the key is held.
    vxe::f64 m_eyeHeight = 1.62;  // Eased, so crouching is not a jump.
    vxe::f64 m_sinceJumpPress = 10.0;
    vxe::f64 m_walked = 0.0; // Blocks, for the view bobbing.
    vxe::f64 m_bob = 0.0;    // How strong the bobbing is now, 0 to 1.
    bool m_sprintLatched = false;
    bool m_crouchLatched = false;
    bool m_jumpWaiting = false; // Pressed since the last step.
};

// Targets the block under the crosshair and breaks, places and picks blocks
// (G6.3). Every change goes through `setBlock`.
//
// Thread safety: main thread only.
class BlockInteraction {
public:
    using SetBlock = std::function<bool(const vxe::BlockPos&, vxe::voxel::BlockStateId)>;

    BlockInteraction(vxe::input::InputSystem& input, const GameActions& actions, const BlockCatalog& catalog)
        : m_input(input)
        , m_actions(actions)
        , m_catalog(catalog) {}

    // One frame of play: finds the target, outlines it and acts on the keys.
    void update(const vxe::render::Camera& camera, const PlayerState& player,
                const vxe::voxel::VoxelWorld& world, Hotbar& hotbar, const SetBlock& setBlock,
                vxe::render::DebugDraw* draw, vxe::f64 deltaSeconds);
    [[nodiscard]] const std::optional<vxe::RaycastHit>& target() const noexcept { return m_target; }

private:
    vxe::input::InputSystem& m_input;
    const GameActions& m_actions;
    const BlockCatalog& m_catalog;
    std::optional<vxe::RaycastHit> m_target;
    vxe::f64 m_breakWait = 0.0; // Seconds until holding the key acts again.
    vxe::f64 m_placeWait = 0.0;
};

// Whether a block placed at `block` would stand inside the player.
[[nodiscard]] bool overlapsPlayer(const PlayerState& player, const vxe::BlockPos& block) noexcept;

} // namespace vr
