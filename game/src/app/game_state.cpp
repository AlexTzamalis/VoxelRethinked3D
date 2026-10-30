#include "game_state.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <utility>

namespace vr {
namespace {

using S = GameState;

// The arrows of Section 20.1, plus the backing out and quick start edges
// described at isAllowedTransition.
using Edge = std::pair<GameState, GameState>;

constexpr std::array kEdges = {
    Edge{S::Boot, S::LoadingCoreAssets},
    Edge{S::LoadingCoreAssets, S::MainMenu},
    Edge{S::LoadingCoreAssets, S::MainMenu},
    Edge{S::LoadingCoreAssets, S::StartingIntegratedServer}, // Without a window there is no menu.
    Edge{S::MainMenu, S::StartingIntegratedServer},          // Singleplayer, until the world list (G5.1).
    Edge{S::MainMenu, S::Settings},
    Edge{S::Settings, S::MainMenu},
    Edge{S::MainMenu, S::ResourcePacks},
    Edge{S::ResourcePacks, S::MainMenu},
    Edge{S::MainMenu, S::WorldSelect},
    Edge{S::WorldSelect, S::MainMenu},
    Edge{S::WorldSelect, S::CreateWorld},
    Edge{S::CreateWorld, S::WorldSelect},
    Edge{S::WorldSelect, S::StartingIntegratedServer},
    Edge{S::CreateWorld, S::StartingIntegratedServer},
    Edge{S::StartingIntegratedServer, S::LoadingWorld},
    Edge{S::StartingIntegratedServer, S::MainMenu}, // The server failed to start.
    Edge{S::LoadingWorld, S::InGame},
    Edge{S::LoadingWorld, S::SavingAndStopping}, // Loading cancelled.
    Edge{S::InGame, S::Paused},
    Edge{S::Paused, S::InGame},
    Edge{S::InGame, S::SavingAndStopping},
    Edge{S::Paused, S::SavingAndStopping},
    Edge{S::SavingAndStopping, S::MainMenu},
};

} // namespace

std::string_view nameOf(GameState state) noexcept {
    constexpr std::array<std::string_view, kGameStateCount> kNames = {
        "Boot",          "LoadingCoreAssets",
        "MainMenu",      "Settings",
        "ResourcePacks", "WorldSelect",
        "CreateWorld",   "StartingIntegratedServer",
        "LoadingWorld",  "InGame",
        "Paused",        "SavingAndStopping"};
    const auto index = static_cast<vxe::usize>(state);
    return index < kNames.size() ? kNames.at(index) : std::string_view("Unknown");
}

bool isAllowedTransition(GameState from, GameState to) noexcept {
    return std::ranges::any_of(kEdges,
                               [&](const Edge& edge) { return edge.first == from && edge.second == to; });
}

bool isMenuState(GameState state) noexcept {
    constexpr std::array kMenus = {S::MainMenu,    S::Settings,    S::ResourcePacks,
                                   S::WorldSelect, S::CreateWorld, S::Paused};
    return std::ranges::find(kMenus, state) != kMenus.end();
}

bool isWorldState(GameState state) noexcept {
    return state == S::InGame || state == S::Paused;
}

bool GameStateMachine::request(GameState next) {
    if (!isAllowedTransition(m_state, next)) {
        return false;
    }
    const GameState previous = std::exchange(m_state, next);
    ++m_transitions;
    if (m_listener) {
        m_listener(previous, next);
    }
    return true;
}

} // namespace vr
