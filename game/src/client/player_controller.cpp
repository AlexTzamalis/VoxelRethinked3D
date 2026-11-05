#include "player_controller.hpp"

#include <vxe/math/face.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vr {
namespace {

using vxe::BlockPos;
using vxe::f32;
using vxe::f64;
using vxe::i64;
using vxe::voxel::BlockStateId;
using vxe::voxel::kAirState;

constexpr f64 kStandingEyes = 1.62;
constexpr f64 kCrouchingEyes = 1.27;
constexpr f64 kMaxFrameSeconds = 0.25;   // A longer frame does not fast forward.
constexpr f64 kDoubleTapSeconds = 0.3;   // Two jump presses this close toggle flight.
constexpr f64 kReach = 5.5;              // Blocks from the eyes.
constexpr f64 kFirstRepeatSeconds = 0.3; // Holding break or place repeats.
constexpr f64 kRepeatSeconds = 0.2;
constexpr f32 kMaxPitch = 1.553F; // Just under straight up, so the view never flips.
constexpr f32 kRadiansPerPixel = 0.0026F;
constexpr f32 kMinZoom = 1.5F;
constexpr f32 kMaxZoom = 12.0F;
constexpr f32 kZoomWheelStep = 1.2F;
constexpr BlockStateId kUnloaded = 0xFFFF;
constexpr vxe::u32 kOutlineColor = vxe::render::debugColor(0, 0, 0, 255);

} // namespace

bool overlapsPlayer(const PlayerState& player, const BlockPos& block) noexcept {
    const auto x = static_cast<f64>(block.x);
    const auto y = static_cast<f64>(block.y);
    const auto z = static_cast<f64>(block.z);
    return player.position.x + kPlayerHalfWidth > x && player.position.x - kPlayerHalfWidth < x + 1.0 &&
           player.position.y + kPlayerHeight > y && player.position.y < y + 1.0 &&
           player.position.z + kPlayerHalfWidth > z && player.position.z - kPlayerHalfWidth < z + 1.0;
}

void PlayerController::place(const vxe::DVec3& feet, f32 yaw, f32 pitch, bool flying) {
    m_state = PlayerState{.position = feet, .flying = flying};
    m_previous = feet;
    m_accumulator = 0.0;
    m_yaw = yaw;
    m_pitch = pitch;
    m_eyeHeight = kStandingEyes;
}

void PlayerController::applyTo(vxe::render::Camera& camera) const {
    const f64 alpha = m_accumulator / kPlayerStep;
    camera.position = m_previous + ((m_state.position - m_previous) * alpha);
    camera.position.y += m_eyeHeight;
    camera.yaw = m_yaw;
    camera.pitch = m_pitch;
}

bool PlayerController::isZooming() const {
    return m_input.isHeld(m_actions.zoom);
}

void PlayerController::update(vxe::render::Camera& camera, const vxe::voxel::VoxelWorld& world,
                              f64 deltaSeconds, i64 bottom, i64 top, bool look) {
    const auto frame = static_cast<f32>(deltaSeconds);
    const vxe::Vec2 mouse = look ? m_input.mouseDelta() : vxe::Vec2(0.0F);
    const f32 turn = kRadiansPerPixel * m_settings.mouseSensitivity.get();
    m_yaw = std::remainder(m_yaw + (mouse.x * turn), 2.0F * std::numbers::pi_v<f32>);
    m_pitch += mouse.y * turn * (m_settings.invertY.get() ? 1.0F : -1.0F);
    m_pitch = std::clamp(m_pitch, -kMaxPitch, kMaxPitch);

    // The wheel chooses how far to zoom while the key is held.
    const bool zooming = isZooming();
    if (zooming) {
        const f32 wheel = m_input.wheelDelta().y;
        m_zoomTarget = std::clamp(m_zoomTarget * std::pow(kZoomWheelStep, wheel), kMinZoom, kMaxZoom);
    }
    m_zoom += ((zooming ? m_zoomTarget : 1.0F) - m_zoom) * std::min(frame * 14.0F, 1.0F);

    const auto axis = [this](vxe::input::ActionId positive, vxe::input::ActionId negative) {
        return (m_input.isHeld(positive) ? 1.0 : 0.0) - (m_input.isHeld(negative) ? 1.0 : 0.0);
    };
    if (m_settings.toggleSprint.get() && m_input.isPressed(m_actions.sprint)) {
        m_sprintLatched = !m_sprintLatched;
    }
    if (m_settings.toggleCrouch.get() && m_input.isPressed(m_actions.crouch)) {
        m_crouchLatched = !m_crouchLatched;
    }
    // At high frame rates a short press can fall between two steps; it counts
    // until a step has seen it.
    m_jumpWaiting = m_jumpWaiting || m_input.isPressed(m_actions.jump);
    PlayerInput input{
        .forward = axis(m_actions.moveForward, m_actions.moveBack),
        .strafe = axis(m_actions.moveRight, m_actions.moveLeft),
        .yaw = m_yaw,
        .jump = m_input.isHeld(m_actions.jump) || m_jumpWaiting,
        .down = m_settings.toggleCrouch.get() ? m_crouchLatched : m_input.isHeld(m_actions.crouch),
        .sprint = m_settings.toggleSprint.get() ? m_sprintLatched : m_input.isHeld(m_actions.sprint)};

    m_sinceJumpPress += deltaSeconds;
    if (m_input.isPressed(m_actions.jump)) {
        if (m_sinceJumpPress < kDoubleTapSeconds) {
            m_state.flying = !m_state.flying;
            m_state.velocity.y = 0.0;
            m_sinceJumpPress = 10.0;
        } else {
            m_sinceJumpPress = 0.0;
        }
    }

    // Above the world is open air and below it a floor. Unloaded chunks count
    // as solid, so the player waits at their edge instead of falling through.
    const auto isSolid = [&](const BlockPos& block) {
        if (block.y >= top) {
            return 0.0;
        }
        if (block.y < bottom) {
            return 1.0;
        }
        const BlockStateId state = world.getBlock(block, kUnloaded);
        return state == kUnloaded ? 1.0 : m_catalog.collisionHeight(state);
    };
    const auto isLiquid = [&](const BlockPos& block) {
        const BlockStateId state = world.getBlock(block, kAirState);
        return m_catalog.isLiquid(state);
    };
    m_accumulator += std::min(deltaSeconds, kMaxFrameSeconds);
    while (m_accumulator >= kPlayerStep) {
        m_accumulator -= kPlayerStep;
        m_previous = m_state.position;
        stepPlayer(m_state, input, isSolid, isLiquid);
        m_jumpWaiting = false;
        input.jump = m_input.isHeld(m_actions.jump);
    }

    const f64 wantEyes = m_state.crouching ? kCrouchingEyes : kStandingEyes;
    m_eyeHeight += (wantEyes - m_eyeHeight) * std::min(deltaSeconds * 14.0, 1.0);
    applyTo(camera);

    // A small sway while walking, none in flight.
    const f64 speed = std::hypot(m_state.velocity.x, m_state.velocity.z);
    const bool walking = m_state.onGround && speed > 0.5;
    m_walked += speed * deltaSeconds;
    m_bob += ((walking ? 1.0 : 0.0) - m_bob) * std::min(deltaSeconds * 10.0, 1.0);
    if (m_settings.viewBobbing.get()) {
        constexpr f64 kBobHeight = 0.035;
        constexpr f64 kBobPerBlock = 3.2;
        camera.position.y += std::abs(std::sin(m_walked * kBobPerBlock)) * kBobHeight * m_bob;
    }
}

// Removes a block with what cannot stay without it: the other half of a tall
// plant, and a plant standing on it.
void BlockInteraction::removeBlock(const vxe::voxel::VoxelWorld& world, const BlockPos& block,
                                   const SetBlock& setBlock) const {
    const BlockStateId state = world.getBlock(block, kAirState);
    if (m_catalog.tallLower(state) != kAirState) {
        static_cast<void>(setBlock({.x = block.x, .y = block.y - 1, .z = block.z}, kAirState));
    }
    static_cast<void>(setBlock(block, kAirState));
    BlockPos above{.x = block.x, .y = block.y + 1, .z = block.z};
    while (m_catalog.needsSupport(world.getBlock(above, kAirState))) {
        static_cast<void>(setBlock(above, kAirState));
        ++above.y;
    }
}

void BlockInteraction::update(const vxe::render::Camera& camera, const PlayerState& player,
                              const vxe::voxel::VoxelWorld& world, Hotbar& hotbar, const SetBlock& setBlock,
                              vxe::render::DebugDraw* draw, f64 deltaSeconds) {
    const auto targetable = [&](const BlockPos& block) {
        const BlockStateId state = world.getBlock(block, kAirState);
        return state != kAirState && !m_catalog.isLiquid(state);
    };
    m_target = vxe::raycastBlocks(camera.position, vxe::DVec3(camera.forward()), kReach, targetable);
    if (!m_target) {
        m_breakWait = 0.0;
        m_placeWait = 0.0;
        return;
    }
    const BlockPos block = m_target->block;
    if (draw != nullptr) {
        constexpr f64 kGrow = 0.003; // Off the faces, so the lines do not flicker.
        const vxe::DVec3 corner(static_cast<f64>(block.x), static_cast<f64>(block.y),
                                static_cast<f64>(block.z));
        const f64 height = m_catalog.shapeHeight(world.getBlock(block, kAirState));
        draw->box(corner - kGrow, corner + vxe::DVec3(1.0, height, 1.0) + kGrow, kOutlineColor);
    }

    // A press acts at once; holding repeats after a pause.
    const auto shouldAct = [&](vxe::input::ActionId action, f64& wait) {
        wait -= deltaSeconds;
        if (m_input.isPressed(action)) {
            wait = kFirstRepeatSeconds;
            return true;
        }
        if (m_input.isHeld(action) && wait <= 0.0) {
            wait = kRepeatSeconds;
            return true;
        }
        return false;
    };
    if (m_input.isPressed(m_actions.pickBlock)) {
        hotbar.slots.at(hotbar.selected) = world.getBlock(block, kAirState);
    }
    if (shouldAct(m_actions.breakBlock, m_breakWait)) {
        removeBlock(world, block, setBlock);
        return;
    }
    if (shouldAct(m_actions.placeBlock, m_placeWait)) {
        const BlockStateId state = hotbar.current();
        const vxe::IVec3 normal = vxe::normalOf(m_target->face);
        // Snow on snow: the top of a layer takes one more, up to a whole block.
        const BlockStateId targetState = world.getBlock(block, kAirState);
        const BlockStateId grown = m_catalog.nextLayer(targetState);
        if (grown != kAirState && m_target->face == vxe::Face::PosY &&
            m_catalog.sameBlock(state, targetState)) {
            if (!overlapsPlayer(player, block)) {
                static_cast<void>(setBlock(block, grown));
            }
            return;
        }
        // Pointing at grass or a flower means the place it stands on: a block
        // goes where the plant is, not beside it, so foliage never gets in the
        // way of building.
        const bool intoTarget = m_catalog.isReplaceable(world.getBlock(block, kAirState));
        const BlockPos place =
            intoTarget ? block
                       : BlockPos{.x = block.x + normal.x, .y = block.y + normal.y, .z = block.z + normal.z};
        const BlockStateId there = world.getBlock(place, kUnloaded);
        const bool free =
            there == kAirState ||
            (there != kUnloaded && (m_catalog.isLiquid(there) || m_catalog.isReplaceable(there)));
        // Plants grow on solid ground, and tall ones need room above.
        const BlockPos below{.x = place.x, .y = place.y - 1, .z = place.z};
        const BlockPos above{.x = place.x, .y = place.y + 1, .z = place.z};
        const BlockStateId upper = m_catalog.tallUpper(state);
        // Plants do not grow under water: nothing here keeps the water around them.
        const bool grounded =
            !m_catalog.needsSupport(state) ||
            (m_catalog.isSolid(world.getBlock(below, kAirState)) && !m_catalog.isLiquid(there));
        const bool roomy = upper == kAirState || world.getBlock(above, kUnloaded) == kAirState;
        if (state != kAirState && free && grounded && roomy &&
            (!m_catalog.isSolid(state) || !overlapsPlayer(player, place))) {
            if (there != kAirState) {
                removeBlock(world, place, setBlock); // A plant being replaced goes with its other half.
            }
            static_cast<void>(setBlock(place, state));
            if (upper != kAirState) {
                static_cast<void>(setBlock(above, upper));
            }
        }
    }
}

} // namespace vr
