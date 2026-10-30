#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/ui/screen.hpp>

#include <functional>

namespace vr {

// The pause menu over the world (Section 19.3): back to the game, the
// settings and leaving. Back (Escape) returns to the game. Saving
// and quitting to the title arrive with the main menu (G4.1) and saving
// (G5.4); for now Quit closes the game.
class PauseScreen final : public vxe::ui::Screen {
public:
    PauseScreen(vxe::config::SettingsRegistry& settings, std::function<void()> resume,
                std::function<void()> quit)
        : m_settings(settings)
        , m_resume(std::move(resume))
        , m_quit(std::move(quit)) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;
    bool onBack() override;

private:
    vxe::config::SettingsRegistry& m_settings;
    std::function<void()> m_resume;
    std::function<void()> m_quit;
};

} // namespace vr
