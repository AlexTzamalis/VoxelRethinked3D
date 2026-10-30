#pragma once

#include "menu_services.hpp"

#include <vxe/ui/screen.hpp>

#include <functional>

namespace vr {

// The pause menu over the world (Section 19.3): back to the game, the
// settings and quitting to the title. Back (Escape) returns to the game.
// Saving on the way out arrives with G5.4.
class PauseScreen final : public vxe::ui::Screen {
public:
    PauseScreen(const MenuServices& services, std::function<void()> resume, std::function<void()> quitToTitle)
        : m_services(services)
        , m_resume(std::move(resume))
        , m_quitToTitle(std::move(quitToTitle)) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;
    bool onBack() override;

private:
    MenuServices m_services;
    std::function<void()> m_resume;
    std::function<void()> m_quitToTitle;
};

} // namespace vr
