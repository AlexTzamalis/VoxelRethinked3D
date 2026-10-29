#pragma once

#include <vxe/core/types.hpp>

#include <functional>
#include <string_view>
#include <utility>

namespace vr {

// The states of the game application (Section 20.1):
//
//   Boot -> LoadingCoreAssets -> MainMenu <-> (Settings, ResourcePacks, WorldSelect -> CreateWorld)
//                                    |
//                                    v
//              StartingIntegratedServer -> LoadingWorld -> InGame <-> Paused
//                                                            |
//                                                            v
//                                               SavingAndStopping -> MainMenu
enum class GameState : vxe::u8 {
    Boot,
    LoadingCoreAssets,
    MainMenu,
    Settings,
    ResourcePacks,
    WorldSelect,
    CreateWorld,
    StartingIntegratedServer,
    LoadingWorld,
    InGame,
    Paused,
    SavingAndStopping,
};

inline constexpr vxe::usize kGameStateCount = 12;

[[nodiscard]] std::string_view nameOf(GameState state) noexcept;

// Whether the diagram above allows going from `from` to `to`. Besides the
// arrows of Section 20.1 it allows backing out of a failed or cancelled
// world start to the main menu, and, until the menus exist (G4), going
// straight from LoadingCoreAssets to StartingIntegratedServer (the demo
// world start agreed for the first playable build).
[[nodiscard]] bool isAllowedTransition(GameState from, GameState to) noexcept;

// States in which a menu or screen has the input (the frame rate cap for
// menus applies and the mouse is free).
[[nodiscard]] bool isMenuState(GameState state) noexcept;
// States with a world loaded (the server runs, the world renders).
[[nodiscard]] bool isWorldState(GameState state) noexcept;

// The application's current state. Transitions happen at once when
// requested, and only along the allowed arrows; a listener hears about
// every transition (to push input contexts, start the server and so on).
//
// Thread safety: main thread only.
class GameStateMachine {
public:
    using Listener = std::function<void(GameState from, GameState to)>;

    [[nodiscard]] GameState current() const noexcept { return m_state; }
    // Moves to `next` if the transition is allowed. Returns false (and
    // changes nothing) otherwise.
    bool request(GameState next);
    void setListener(Listener listener) { m_listener = std::move(listener); }
    [[nodiscard]] vxe::u64 transitions() const noexcept { return m_transitions; }

private:
    GameState m_state = GameState::Boot;
    Listener m_listener;
    vxe::u64 m_transitions = 0;
};

} // namespace vr
