#pragma once

#include "common/player_physics.hpp"

#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/entity_renderer.hpp>

#include <vector>

namespace vr {

// What the held item looks like: nothing, a cube with six faces, or a flat
// picture (plants), from the entity texture's item layers.
enum class HeldItem : vxe::u8 {
    None,
    Block,
    Flat,
};

// How the player stands this frame, from which the model is posed.
struct PlayerPose {
    vxe::DVec3 feet{};       // World position of the feet (between the two legs).
    vxe::f32 headYaw = 0.0F; // Radians, as render::Camera::yaw.
    vxe::f32 pitch = 0.0F;   // Radians, up positive.
    bool onGround = true;
    bool flying = false;
    bool crouching = false;
    HeldItem held = HeldItem::None;
    vxe::f32 light = 1.0F; // Brightness where the player stands, 0 to 1.
};

// The player's body, animated like Minecraft's: legs that swing with the
// distance walked, arms that swing the other way and sway a little at rest,
// a head that turns and tilts with the view and a body that follows it once
// it turns too far or the player moves, a crouch, and the arm swing of
// breaking and placing blocks. The skin is a 64 x 64 image in Minecraft's
// layout, so any such skin fits.
//
// Model units are skin pixels (16 to a block), feet at 0, facing -Z with the
// player's right towards +X; the whole body is 32 pixels (2 blocks) tall.
//
// Thread safety: main thread only.
class PlayerModel {
public:
    // One frame of animation: how far the feet moved and the time passed.
    void update(const PlayerPose& pose, vxe::f64 deltaSeconds);
    // Starts the arm swing of breaking or placing a block.
    void swing() noexcept {
        m_swing = 0.0F;
        m_swinging = true;
    }
    // Forgets the walk, for a teleport or a new world.
    void reset() noexcept { m_started = false; }

    // The body in the world (third person), camera relative.
    void buildBody(const PlayerPose& pose, const vxe::render::Camera& camera,
                   std::vector<vxe::render::EntityVertex>& out) const;
    // The right arm, or the item in hand, at the bottom right of the view.
    void buildFirstPerson(const PlayerPose& pose, const vxe::render::Camera& camera,
                          std::vector<vxe::render::EntityVertex>& out) const;

private:
    bool m_started = false;
    vxe::DVec3 m_lastFeet{};
    vxe::f32 m_bodyYaw = 0.0F;
    vxe::f32 m_walkPhase = 0.0F;  // Radians of the leg swing.
    vxe::f32 m_walkAmount = 0.0F; // 0 standing to 1 walking or faster.
    vxe::f32 m_age = 0.0F;        // Seconds, for the resting sway.
    vxe::f32 m_swing = 1.0F;      // 0 to 1 through the arm swing.
    bool m_swinging = false;
};

} // namespace vr
