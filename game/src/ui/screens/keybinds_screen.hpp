#pragma once

#include "menu_services.hpp"

#include <vxe/ui/screen.hpp>

namespace vr {

// The controls screen (Section 19.3): the engine's keybind list in a panel
// with a Done button. Escape leaves it, unless a key is being captured,
// where Escape only cancels the capture.
class KeybindsScreen final : public vxe::ui::Screen {
public:
    explicit KeybindsScreen(const MenuServices& services)
        : m_services(services) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override;

private:
    MenuServices m_services;
};

} // namespace vr
