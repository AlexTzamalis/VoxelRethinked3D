#pragma once

#include "client/block_icons.hpp"
#include "client/player_controller.hpp"
#include "common/block_catalog.hpp"

#include <vxe/ui/screen.hpp>
#include <vxe/ui/widget.hpp>

#include <string>

namespace vr {

// The crosshair and the hotbar, drawn from pack images (ui/crosshair.png,
// ui/hotbar_slot.png, ui/hotbar_selected.png) with the blocks' icons. The
// selected block's name shows above the bar for a moment after it changes.
class HudWidget final : public vxe::ui::Widget {
public:
    HudWidget(const Hotbar& hotbar, const BlockIcons& icons, const BlockCatalog& catalog)
        : m_hotbar(hotbar)
        , m_icons(icons)
        , m_catalog(catalog) {}

    // Brings the widget up to date; redraws only after a change.
    void sync(bool shown, vxe::f32 opacity, vxe::f32 deltaSeconds);
    void draw(vxe::ui::DrawContext& context) const override;

private:
    const Hotbar& m_hotbar;
    const BlockIcons& m_icons;
    const BlockCatalog& m_catalog;
    Hotbar m_drawn;
    bool m_shown = true;
    vxe::f32 m_opacity = 1.0F;
    vxe::f32 m_nameSeconds = 0.0F; // How much longer the name shows.
    std::string m_name;
};

// In the world while playing: no cursor, no backdrop, only the HUD.
class HudScreen final : public vxe::ui::Screen {
public:
    HudScreen(const Hotbar& hotbar, const BlockIcons& icons, const BlockCatalog& catalog)
        : m_hotbar(hotbar)
        , m_icons(icons)
        , m_catalog(catalog) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override {
        return {.drawBelow = false, .backdrop = false, .cursor = false};
    }
    void build(vxe::ui::Widget& root) override;
    [[nodiscard]] HudWidget* hud() const noexcept { return m_hud; }

private:
    const Hotbar& m_hotbar;
    const BlockIcons& m_icons;
    const BlockCatalog& m_catalog;
    HudWidget* m_hud = nullptr;
};

} // namespace vr
