#pragma once

#include "client/block_icons.hpp"
#include "client/player_controller.hpp"
#include "common/block_catalog.hpp"
#include "menu_services.hpp"

#include <vxe/ui/screen.hpp>
#include <vxe/ui/widgets.hpp>

#include <string>
#include <vector>

namespace vr {

// A button showing a block icon instead of text.
class IconButton final : public vxe::ui::Button {
public:
    IconButton(std::string icon, std::function<void()> onClick)
        : Button({}, std::move(onClick))
        , m_icon(std::move(icon)) {}
    void setIcon(std::string icon) {
        if (icon != m_icon) {
            m_icon = std::move(icon);
            invalidateVisual();
        }
    }
    void draw(vxe::ui::DrawContext& context) const override;

private:
    std::string m_icon;
};

// The creative inventory (G6.4b): E shows every block as an icon, with a
// search box and categories. Clicking a block puts it in the selected hotbar
// slot; clicking a slot of the bar below selects it. The world keeps running
// behind it. E or Escape closes it, and keyboards reach every button.
class InventoryScreen final : public vxe::ui::Screen {
public:
    InventoryScreen(const MenuServices& services, const BlockCatalog& catalog, const BlockIcons& icons,
                    Hotbar& hotbar)
        : m_services(services)
        , m_catalog(catalog)
        , m_icons(icons)
        , m_hotbar(hotbar) {}

    [[nodiscard]] vxe::ui::ScreenOptions options() const override {
        return {.drawBelow = true, .backdrop = true};
    }
    void build(vxe::ui::Widget& root) override;
    void update(vxe::f32 deltaSeconds) override;

private:
    void showCategory(std::string category);
    void rebuildGrid();
    void syncHotbar();

    MenuServices m_services;
    const BlockCatalog& m_catalog;
    const BlockIcons& m_icons;
    Hotbar& m_hotbar;
    std::string m_category;     // Empty for all.
    bool m_keyReleased = false; // The E that opened the screen has been let go.
    vxe::ui::TextField* m_search = nullptr;
    vxe::ui::Widget* m_grid = nullptr;
    std::vector<std::pair<std::string, vxe::ui::Button*>> m_tabs;
    std::vector<IconButton*> m_slots;
};

} // namespace vr
