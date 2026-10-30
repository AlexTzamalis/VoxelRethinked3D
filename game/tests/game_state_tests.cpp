#include "app/game_state.hpp"

#include <doctest/doctest.h>

#include <utility>
#include <vector>

using vr::GameState;

TEST_CASE("The game starts in Boot and follows the Section 20.1 arrows") {
    vr::GameStateMachine states;
    std::vector<std::pair<GameState, GameState>> seen;
    states.setListener([&](GameState from, GameState to) { seen.emplace_back(from, to); });
    CHECK(states.current() == GameState::Boot);

    CHECK_FALSE(states.request(GameState::InGame)); // No skipping ahead.
    CHECK(states.current() == GameState::Boot);
    CHECK(seen.empty());

    // Launch to the menu, into the world and back to the title.
    for (const GameState next :
         {GameState::LoadingCoreAssets, GameState::MainMenu, GameState::StartingIntegratedServer,
          GameState::LoadingWorld, GameState::InGame, GameState::Paused, GameState::InGame, GameState::Paused,
          GameState::SavingAndStopping, GameState::MainMenu}) {
        REQUIRE(states.request(next));
        CHECK(states.current() == next);
    }
    CHECK(states.transitions() == 10);
    REQUIRE(seen.size() == 10);
    CHECK(seen[1] == std::pair{GameState::LoadingCoreAssets, GameState::MainMenu});

    // Menus.
    CHECK(states.request(GameState::WorldSelect));
    CHECK(states.request(GameState::CreateWorld));
    CHECK_FALSE(states.request(GameState::Paused));
    CHECK(states.request(GameState::StartingIntegratedServer));
    CHECK(states.request(GameState::MainMenu));   // The server failed to start.
    CHECK_FALSE(states.request(GameState::Boot)); // Never back to Boot.
}

TEST_CASE("Every state has a name and a menu and world classification") {
    for (vxe::usize i = 0; i < vr::kGameStateCount; ++i) {
        const auto state = static_cast<GameState>(i);
        CHECK_FALSE(vr::nameOf(state).empty());
        CHECK(vr::nameOf(state) != "Unknown");
        CHECK_FALSE(vr::isAllowedTransition(state, state)); // No self loops.
    }
    CHECK(vr::isMenuState(GameState::Paused));
    CHECK(vr::isMenuState(GameState::MainMenu));
    CHECK_FALSE(vr::isMenuState(GameState::InGame));
    CHECK(vr::isWorldState(GameState::InGame));
    CHECK(vr::isWorldState(GameState::Paused));
    CHECK_FALSE(vr::isWorldState(GameState::LoadingWorld));
}

TEST_CASE("Every state can be reached from Boot") {
    std::vector<bool> reached(vr::kGameStateCount, false);
    reached[0] = true;
    for (vxe::usize round = 0; round < vr::kGameStateCount; ++round) {
        for (vxe::usize from = 0; from < vr::kGameStateCount; ++from) {
            for (vxe::usize to = 0; to < vr::kGameStateCount; ++to) {
                if (reached[from] &&
                    vr::isAllowedTransition(static_cast<GameState>(from), static_cast<GameState>(to))) {
                    reached[to] = true;
                }
            }
        }
    }
    for (vxe::usize i = 0; i < vr::kGameStateCount; ++i) {
        INFO(vr::nameOf(static_cast<GameState>(i)));
        CHECK(reached[i]);
    }
}
