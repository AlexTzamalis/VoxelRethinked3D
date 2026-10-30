#include "common/player_physics.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace vr;
using vxe::BlockPos;
using vxe::DVec3;

namespace {

constexpr vxe::f64 kCenter = 0.5;

// A flat floor: everything below y = 0 is solid.
bool floorOnly(const BlockPos& block) {
    return block.y < 0;
}

bool noLiquid(const BlockPos& /*block*/) {
    return false;
}

template <typename Solid>
void run(PlayerState& state, const PlayerInput& input, vxe::f64 seconds, const Solid& solid) {
    for (int i = 0; i < static_cast<int>(seconds / kPlayerStep); ++i) {
        stepPlayer(state, input, solid, noLiquid);
    }
}

} // namespace

TEST_CASE("The player falls, lands and stands still on the floor") {
    PlayerState state{.position = {kCenter, 5.0, kCenter}};
    run(state, {}, 2.0, floorOnly);
    CHECK(state.onGround);
    CHECK(state.position.y == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(state.velocity.y == 0.0);
    const DVec3 landed = state.position;
    run(state, {}, 1.0, floorOnly);
    CHECK(state.position.y == doctest::Approx(landed.y).epsilon(1e-9));
}

TEST_CASE("A jump clears one block but not two") {
    PlayerState state{.position = {kCenter, 0.0, kCenter}, .onGround = true};
    vxe::f64 highest = 0.0;
    stepPlayer(state, {.jump = true}, floorOnly, noLiquid);
    for (int i = 0; i < 90; ++i) {
        stepPlayer(state, {}, floorOnly, noLiquid);
        highest = std::max(highest, state.position.y);
    }
    CHECK(highest > 1.0);
    CHECK(highest < 2.0);
}

TEST_CASE("Walking reaches its speed and walls stop it") {
    PlayerState state{.position = {kCenter, 0.0, kCenter}, .onGround = true};
    // Yaw 0 looks along -Z.
    run(state, {.forward = 1.0}, 1.0, floorOnly);
    const vxe::f64 walked = kCenter - state.position.z;
    CHECK(walked > 3.5);
    CHECK(walked < 4.3);
    CHECK(state.position.x == doctest::Approx(kCenter));

    const auto wall = [](const BlockPos& block) { return block.y < 0 || block.z == -3; };
    PlayerState blocked{.position = {kCenter, 0.0, kCenter}, .onGround = true};
    run(blocked, {.forward = 1.0}, 3.0, wall);
    CHECK(blocked.position.z == doctest::Approx(-2.0 + kPlayerHalfWidth).epsilon(1e-3));
    CHECK(blocked.velocity.z == 0.0);

    // A wall only stops the axis that hits it: the player slides along it.
    PlayerState sliding{.position = {kCenter, 0.0, kCenter}, .onGround = true};
    run(sliding, {.forward = 1.0, .strafe = 1.0}, 2.0, wall);
    CHECK(sliding.position.x > 2.0);
    CHECK(sliding.position.z == doctest::Approx(-2.0 + kPlayerHalfWidth).epsilon(1e-3));
}

TEST_CASE("No speed tunnels through a block") {
    const auto thin = [](const BlockPos& block) { return block.y == -100; };
    PlayerState state{.position = {kCenter, 200.0, kCenter}, .velocity = {0.0, -78.0, 0.0}};
    run(state, {}, 10.0, thin);
    CHECK(state.onGround);
    CHECK(state.position.y == doctest::Approx(-99.0).epsilon(1e-3));
}

TEST_CASE("Ceilings stop a jump") {
    const auto ceiling = [](const BlockPos& block) { return block.y < 0 || block.y == 2; };
    PlayerState state{.position = {kCenter, 0.0, kCenter}, .onGround = true};
    stepPlayer(state, {.jump = true}, ceiling, noLiquid);
    vxe::f64 highest = 0.0;
    for (int i = 0; i < 60; ++i) {
        stepPlayer(state, {}, ceiling, noLiquid);
        highest = std::max(highest, state.position.y + kPlayerHeight);
    }
    CHECK(highest <= 2.0);
    CHECK(state.onGround);
}

TEST_CASE("Crouching stops at edges, walking does not") {
    const auto ledge = [](const BlockPos& block) { return block.y < 0 && block.z >= 0; };
    PlayerState crouched{.position = {kCenter, 0.0, 2.5}, .onGround = true};
    run(crouched, {.forward = 1.0, .down = true}, 5.0, ledge);
    CHECK(crouched.onGround);
    CHECK(crouched.position.y == doctest::Approx(0.0).epsilon(1e-3));
    CHECK(crouched.position.z > -kPlayerHalfWidth);

    PlayerState walking{.position = {kCenter, 0.0, 2.5}, .onGround = true};
    run(walking, {.forward = 1.0}, 3.0, ledge);
    CHECK(walking.position.y < -1.0);
}

TEST_CASE("Flight ignores gravity and still collides") {
    PlayerState state{.position = {kCenter, 5.0, kCenter}, .flying = true};
    run(state, {}, 2.0, floorOnly);
    CHECK(state.position.y == doctest::Approx(5.0));
    run(state, {.jump = true}, 1.0, floorOnly);
    CHECK(state.position.y > 11.0);
    run(state, {.down = true}, 5.0, floorOnly);
    CHECK(state.position.y == doctest::Approx(0.0).epsilon(1e-3));
    CHECK_FALSE(state.crouching);
}

TEST_CASE("Water sinks slowly and swims up") {
    const auto water = [](const BlockPos& block) { return block.y >= 0 && block.y < 20; };
    PlayerState state{.position = {kCenter, 10.0, kCenter}};
    for (int i = 0; i < 60; ++i) {
        stepPlayer(state, {}, floorOnly, water);
    }
    CHECK(state.position.y > 7.0);
    CHECK(state.position.y < 10.0);
    const vxe::f64 sunk = state.position.y;
    for (int i = 0; i < 60; ++i) {
        stepPlayer(state, {.jump = true}, floorOnly, water);
    }
    CHECK(state.position.y > sunk + 2.0);
}
