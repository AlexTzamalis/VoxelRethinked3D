#include "client/player_controller.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/input/input.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <array>

namespace {

using vxe::platform::KeyEvent;
using vxe::platform::Modifiers;
using vxe::platform::PlatformEvent;
namespace scancode = vxe::input::scancode;

} // namespace

TEST_CASE("Developer mode flies fast straight through rock; creative stays stuck in it") {
    auto loaded = vr::BlockCatalog::loadFolder(VR_BLOCKS_DIR);
    REQUIRE(loaded.hasValue());
    const vr::BlockCatalog& catalog = loaded.value();
    const auto stone = catalog.stateId("stone");
    REQUIRE(stone.has_value());
    // Solid stone for 32 blocks in every direction around the start.
    vxe::voxel::VoxelWorld world;
    for (vxe::i32 z = -6; z <= 1; ++z) {
        for (vxe::i32 y = -2; y <= 1; ++y) {
            for (vxe::i32 x = -2; x <= 1; ++x) {
                vxe::voxel::ChunkStorage storage;
                storage.fill(stone.value_or(vxe::voxel::kAirState));
                world.insertChunk({.x = x, .y = y, .z = z}, std::move(storage));
            }
        }
    }

    vxe::config::SettingsRegistry registry;
    const vr::GameSettings settings(registry);
    vxe::input::InputSystem input;
    const vr::GameActions actions(input);
    input.pushContext(actions.gameplay);
    vr::PlayerController player(input, actions, settings, catalog);
    vxe::render::Camera camera;

    // Two seconds with W held, looking along -Z (yaw 0).
    const auto fly = [&](vr::GameMode mode) {
        REQUIRE(registry.set("Gameplay.GameMode", vxe::config::SettingValue{static_cast<vxe::i32>(mode)})
                    .hasValue());
        player.place({0.5, 4.0, 0.5}, 0.0F, 0.0F, false);
        const std::array<PlatformEvent, 1> press{
            KeyEvent{.scancode = scancode::kW, .down = true, .repeat = false, .modifiers = Modifiers::None}};
        input.update(press);
        for (int frame = 0; frame < 120; ++frame) {
            player.update(camera, world, 1.0 / 60.0, -1024, 3072);
            input.update({});
        }
        const std::array<PlatformEvent, 1> release{
            KeyEvent{.scancode = scancode::kW, .down = false, .repeat = false, .modifiers = Modifiers::None}};
        input.update(release);
        return player.state();
    };

    const vr::PlayerState stuck = fly(vr::GameMode::Creative);
    CHECK(stuck.position.z == doctest::Approx(0.5));
    CHECK(stuck.position.y == doctest::Approx(4.0));

    const vr::PlayerState developer = fly(vr::GameMode::Developer);
    CHECK(developer.flying);
    CHECK(developer.position.z < -60.0); // About 44 blocks a second once at speed.
    CHECK(developer.position.y == doctest::Approx(4.0));
}
