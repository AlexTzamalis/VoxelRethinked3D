#pragma once

#include "menu_services.hpp"

#include <vxe/ui/screen.hpp>

#include <functional>

namespace vr {

// The first screen after launch (Section 19.3): the title over the panorama,
// Singleplayer, Multiplayer (disabled), Settings, Resource Packs (until G4.4,
// disabled) and Quit, with the version in a corner. It needs nothing but the
// font, so it shows at once while the world streams in behind it.
class MainMenuScreen final : public vxe::ui::Screen {
public:
    MainMenuScreen(const MenuServices& services, std::function<void()> singleplayer,
                   std::function<void()> quit)
        : m_services(services)
        , m_singleplayer(std::move(singleplayer))
        , m_quit(std::move(quit)) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {}; }
    void build(vxe::ui::Widget& root) override;
    // The menu is the bottom of the stack: Back has nowhere to go.
    bool onBack() override { return true; }

private:
    MenuServices m_services;
    std::function<void()> m_singleplayer;
    std::function<void()> m_quit;
};

} // namespace vr
