#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace vr {

// Player movement (Section 20.4). A plain function of the state, the input and
// the blocks, with a fixed step, so the client and the future server (G0.2)
// run exactly the same code and get exactly the same result.
//
// The numbers are Minecraft's, per tick of 1/20 second: speed is gained by a
// push each tick and lost by friction, so starting and stopping take a moment
// and letting go in the air or in flight glides on. The camera interpolates
// between ticks, so 20 a second still looks smooth.

inline constexpr vxe::f64 kPlayerStep = 1.0 / 20.0; // Seconds per step (one tick).
inline constexpr vxe::f64 kPlayerHalfWidth = 0.3;
inline constexpr vxe::f64 kPlayerHeight = 1.8;

struct PlayerState {
    vxe::DVec3 position{}; // The center of the feet.
    vxe::DVec3 velocity{}; // Blocks per second.
    bool onGround = false;
    bool flying = false;
    bool crouching = false;
};

struct PlayerInput {
    vxe::f64 forward = 0.0; // -1 to 1.
    vxe::f64 strafe = 0.0;  // -1 to 1, positive to the right.
    vxe::f32 yaw = 0.0F;    // Radians, as render::Camera.
    bool jump = false;      // Also up in flight and in water.
    bool down = false;      // Crouch, or down in flight.
    bool sprint = false;
    vxe::f64 walkScale = 1.0; // Times the walking and running speeds.
    vxe::f64 flyScale = 1.0;  // Times the flying speeds.
    bool momentum = true;     // False: full speed at once and a dead stop on letting go.
};

namespace detail {

// Pushes per tick in blocks per tick, and the share of speed kept each tick.
constexpr vxe::f64 kTicksPerSecond = 20.0;
constexpr vxe::f64 kWalkPush = 0.1; // Steady speed 0.1 * 0.98 / (1 - 0.546): 4.317 blocks per second.
constexpr vxe::f64 kSprintFactor = 1.3;
constexpr vxe::f64 kAirPush = 0.02; // In the air: 0.026 sprinting.
constexpr vxe::f64 kFlyPush = 0.05; // Steady 10.9 blocks per second, twice that sprinting.
constexpr vxe::f64 kFlyRise = 0.15; // Up or down per tick in flight, kept at 0.6 a tick: 7.5 per second.
constexpr vxe::f64 kInputScale = 0.98;
constexpr vxe::f64 kCrouchInput = 0.3;
constexpr vxe::f64 kGroundKeep = 0.6 * 0.91; // Friction of ordinary ground.
constexpr vxe::f64 kAirKeep = 0.91;
constexpr vxe::f64 kFlyRiseKeep = 0.6;
constexpr vxe::f64 kGravity = 0.08; // Per tick, then 0.98 of the fall kept: 78.4 blocks per second at most.
constexpr vxe::f64 kFallKeep = 0.98;
constexpr vxe::f64 kJump = 0.42;      // Blocks per tick up: about 1.25 blocks high.
constexpr vxe::f64 kSprintJump = 0.2; // Forward, for a sprinting jump.
constexpr vxe::f64 kWaterKeep = 0.8;
constexpr vxe::f64 kWaterPush = 0.02;
constexpr vxe::f64 kWaterSink = 0.02;
constexpr vxe::f64 kWaterRise = 0.04;
// Keeps the box a hair off the block it touches, so the next step starts in
// free space whatever rounding did.
constexpr vxe::f64 kSkin = 1.0e-6;
// Walking climbs this much without a jump: snow layers, never a whole block.
constexpr vxe::f64 kStepHeight = 0.6;

[[nodiscard]] inline vxe::i64 floorOf(vxe::f64 value) noexcept {
    return static_cast<vxe::i64>(std::floor(value));
}

// A collision callback answers whether a block is solid (bool), or how tall
// its solid part stands from the cell's floor, 0 to 1 (f64: slabs like snow).
[[nodiscard]] inline vxe::f64 heightOf(bool solid) noexcept {
    return solid ? 1.0 : 0.0;
}
[[nodiscard]] inline vxe::f64 heightOf(vxe::f64 height) noexcept {
    return height;
}

// How far the box may move along `axis` (0 X, 1 Y, 2 Z), up to `delta`. Walks
// the layers of blocks in front of the box in order, so no speed can tunnel
// through a block, and stops at the first layer that blocks. A slab blocks a
// fall at its top, and a sideways move only where it reaches above the feet.
template <typename IsSolid>
[[nodiscard]] vxe::f64 sweepAxis(const vxe::DVec3& feet, int axis, vxe::f64 delta, IsSolid&& isSolid) {
    if (delta == 0.0) {
        return 0.0;
    }
    const std::array<vxe::f64, 3> low{feet.x - kPlayerHalfWidth, feet.y, feet.z - kPlayerHalfWidth};
    const std::array<vxe::f64, 3> high{feet.x + kPlayerHalfWidth, feet.y + kPlayerHeight,
                                       feet.z + kPlayerHalfWidth};
    const auto index = static_cast<vxe::usize>(axis);
    const vxe::f64 leading = delta > 0.0 ? high.at(index) : low.at(index);
    const vxe::i64 first = floorOf(leading);
    const vxe::i64 last = floorOf(leading + delta);
    const vxe::i64 step = delta > 0.0 ? 1 : -1;
    const auto a = static_cast<vxe::usize>((axis + 1) % 3);
    const auto b = static_cast<vxe::usize>((axis + 2) % 3);
    const vxe::i64 aFirst = floorOf(low.at(a) + kSkin);
    const vxe::i64 aLast = floorOf(high.at(a) - kSkin);
    const vxe::i64 bFirst = floorOf(low.at(b) + kSkin);
    const vxe::i64 bLast = floorOf(high.at(b) - kSkin);
    for (vxe::i64 layer = first;; layer += step) {
        bool blocked = false;
        vxe::f64 closest = delta;
        for (vxe::i64 i = aFirst; i <= aLast; ++i) {
            for (vxe::i64 j = bFirst; j <= bLast; ++j) {
                std::array<vxe::i64, 3> cell{};
                cell.at(index) = layer;
                cell.at(a) = i;
                cell.at(b) = j;
                const vxe::f64 height =
                    heightOf(isSolid(vxe::BlockPos{.x = cell[0], .y = cell[1], .z = cell[2]}));
                if (height <= 0.0) {
                    continue;
                }
                const vxe::f64 top = static_cast<vxe::f64>(cell[1]) + height;
                if (axis != 1 && height < 1.0 && feet.y >= top - kSkin) {
                    continue; // A slab below the feet.
                }
                vxe::f64 room = 0.0;
                if (delta > 0.0) {
                    room = static_cast<vxe::f64>(layer) - leading - kSkin;
                } else {
                    room = (axis == 1 ? top : static_cast<vxe::f64>(layer + 1)) - leading + kSkin;
                    if (room <= delta) {
                        continue; // A slab whose top the fall does not reach.
                    }
                }
                // Already touching: stay, never get pushed back.
                room = delta > 0.0 ? std::max(room, 0.0) : std::min(room, 0.0);
                closest = blocked ? (delta > 0.0 ? std::min(closest, room) : std::max(closest, room)) : room;
                blocked = true;
            }
        }
        if (blocked) {
            return closest;
        }
        if (layer == last) {
            return delta;
        }
    }
}

// Whether something stands under the box at `feet`, for the crouch edge stop.
template <typename IsSolid>
[[nodiscard]] bool hasGround(const vxe::DVec3& feet, IsSolid&& isSolid) {
    return sweepAxis(feet, 1, -0.6, isSolid) > -0.6;
}

} // namespace detail

// One step of kPlayerStep seconds. `isSolid` and `isLiquid` take a
// vxe::BlockPos; templates, because they run for every block touched.
template <typename IsSolid, typename IsLiquid>
void stepPlayer(PlayerState& state, const PlayerInput& input, IsSolid&& isSolid, IsLiquid&& isLiquid) {
    using namespace detail;
    const vxe::f64 yaw = static_cast<vxe::f64>(input.yaw);
    const vxe::DVec3 forward(std::sin(yaw), 0.0, -std::cos(yaw));
    const vxe::DVec3 right(std::cos(yaw), 0.0, std::sin(yaw));
    vxe::DVec3 wish = (forward * input.forward) + (right * input.strafe);
    if (const vxe::f64 length = std::hypot(wish.x, wish.z); length > 1.0) {
        wish /= length;
    }
    const vxe::BlockPos body{
        .x = floorOf(state.position.x), .y = floorOf(state.position.y + 0.4), .z = floorOf(state.position.z)};
    const bool swimming = !state.flying && isLiquid(body);
    state.crouching = input.down && !state.flying && !swimming;

    // Velocity is kept in blocks per second; the rules below work in blocks per
    // tick, so every push is scaled by kTicksPerSecond.
    const vxe::DVec3 facing = forward;
    wish *= kInputScale * (state.crouching ? kCrouchInput : 1.0);
    const bool sprinting = input.sprint && !state.crouching && input.forward > 0.0;
    vxe::f64 push = 0.0;
    vxe::f64 keep = 0.0;
    if (state.flying) {
        push = kFlyPush * (input.sprint ? 2.0 : 1.0) * input.flyScale;
        keep = kAirKeep;
    } else if (swimming) {
        push = kWaterPush * input.walkScale;
        keep = kWaterKeep;
    } else if (state.onGround) {
        push = kWalkPush * (sprinting ? kSprintFactor : 1.0) * input.walkScale;
        keep = kGroundKeep;
    } else {
        push = kAirPush * (sprinting ? kSprintFactor : 1.0) * input.walkScale;
        keep = kAirKeep;
    }
    if (input.momentum) {
        state.velocity.x += wish.x * push * kTicksPerSecond;
        state.velocity.z += wish.z * push * kTicksPerSecond;
    } else {
        // The speed momentum would settle at, at once. In the air the walking
        // speed holds, so jumps carry as far as they would.
        const vxe::f64 steady = (state.flying || swimming ? push / (1.0 - keep)
                                                          : kWalkPush * (sprinting ? kSprintFactor : 1.0) *
                                                                input.walkScale / (1.0 - kGroundKeep)) *
                                kTicksPerSecond;
        state.velocity.x = wish.x * steady;
        state.velocity.z = wish.z * steady;
    }
    const vxe::f64 vertical = (input.jump ? 1.0 : 0.0) - (input.down ? 1.0 : 0.0);
    if (state.flying) {
        const vxe::f64 rise = kFlyRise * input.flyScale * kTicksPerSecond;
        state.velocity.y =
            input.momentum ? state.velocity.y + (vertical * rise) : vertical * rise / (1.0 - kFlyRiseKeep);
    } else if (swimming) {
        if (input.jump) {
            state.velocity.y += kWaterRise * kTicksPerSecond;
        }
    } else if (input.jump && state.onGround) {
        state.velocity.y = kJump * kTicksPerSecond;
        if (sprinting) {
            state.velocity.x += facing.x * kSprintJump * kTicksPerSecond;
            state.velocity.z += facing.z * kSprintJump * kTicksPerSecond;
        }
    }

    // Y first, so walking off a ledge and landing stay simple; then X and Z
    // apart, which slides the player along walls.
    const vxe::f64 wantY = state.velocity.y * kPlayerStep;
    const vxe::f64 movedY = sweepAxis(state.position, 1, wantY, isSolid);
    state.position.y += movedY;
    state.onGround = wantY < 0.0 && movedY > wantY;
    if (movedY != wantY) {
        state.velocity.y = 0.0;
    }
    const auto moveFlat = [&](int axis, vxe::f64& position, vxe::f64& velocity) {
        vxe::f64 want = velocity * kPlayerStep;
        if (state.crouching && state.onGround) {
            // Crouching never walks off an edge.
            vxe::DVec3 ahead = state.position;
            (axis == 0 ? ahead.x : ahead.z) += want;
            if (!hasGround(ahead, isSolid)) {
                want = 0.0;
            }
        }
        vxe::f64 moved = sweepAxis(state.position, axis, want, isSolid);
        if (moved != want && state.onGround && !state.flying && !swimming) {
            // Blocked on the ground: try again from a step higher, and keep that
            // if it goes further, settling back down onto what was climbed.
            vxe::DVec3 lifted = state.position;
            lifted.y += sweepAxis(lifted, 1, kStepHeight, isSolid);
            const vxe::f64 across = sweepAxis(lifted, axis, want, isSolid);
            if (std::abs(across) > std::abs(moved) + kSkin) {
                (axis == 0 ? lifted.x : lifted.z) += across;
                lifted.y += sweepAxis(lifted, 1, state.position.y - lifted.y, isSolid);
                state.position.y = lifted.y;
                moved = across;
            }
        }
        position += moved;
        if (moved != want || want == 0.0) {
            velocity = 0.0;
        }
    };
    moveFlat(0, state.position.x, state.velocity.x);
    moveFlat(2, state.position.z, state.velocity.z);

    // What carries into the next tick.
    if (input.momentum || state.flying || swimming) {
        state.velocity.x *= input.momentum ? keep : 0.0;
        state.velocity.z *= input.momentum ? keep : 0.0;
    }
    if (state.flying) {
        state.velocity.y *= input.momentum ? kFlyRiseKeep : 0.0;
        // Touching down ends flight, as in creative mode.
        if (state.onGround) {
            state.flying = false;
        }
    } else if (swimming) {
        state.velocity.y = (state.velocity.y * kWaterKeep) - (kWaterSink * kTicksPerSecond);
    } else {
        state.velocity.y = (state.velocity.y - (kGravity * kTicksPerSecond)) * kFallKeep;
    }
}

} // namespace vr
