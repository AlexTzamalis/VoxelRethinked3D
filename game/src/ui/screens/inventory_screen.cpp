#include "inventory_screen.hpp"

#include <vxe/ui/controls.hpp>

#include <algorithm>
#include <cctype>

namespace vr {
namespace {

using vxe::f32;
using vxe::usize;

constexpr f32 kPanelWidth = 340.0F;
constexpr f32 kCell = 22.0F;
constexpr f32 kIcon = 16.0F;
constexpr usize kColumns = 13;
constexpr usize kTabsPerRow = 4;
constexpr vxe::u16 kKeyE = 8;

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

} // namespace

void IconButton::draw(vxe::ui::DrawContext& context) const {
    Button::draw(context);
    if (!m_icon.empty()) {
        const vxe::ui::UiRect& box = rect();
        context.image(
            {box.x + ((box.width - kIcon) / 2.0F), box.y + ((box.height - kIcon) / 2.0F), kIcon, kIcon},
            m_icon);
    }
}

void InventoryScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    root.style().justify = Justify::Center;
    root.style().align = Align::Center;
    auto& panel = root.add<Panel>();
    panel.style().width = kPanelWidth;
    panel.style().maxHeight = 220.0F;
    panel.style().grow = 1.0F;
    panel.style().gap = 3.0F;
    panel.add<Label>("Blocks", TextRole::Title).setAlign(TextAlign::Center);
    m_search = &panel.add<TextField>(std::string(), "Search blocks");
    m_search->setOnChange([this](const std::string& /*text*/) { rebuildGrid(); });

    auto& tabs = panel.add<Widget>();
    tabs.style().gap = 2.0F;
    Widget* tabRow = nullptr;
    usize tabCount = 0;
    const auto addTab = [&](std::string name) {
        if (tabCount++ % kTabsPerRow == 0) {
            tabRow = &tabs.add<Widget>();
            tabRow->style().direction = Direction::Row;
            tabRow->style().gap = 2.0F;
        }
        auto& button = tabRow->add<Button>(name.empty() ? "All" : name, [this, name] { showCategory(name); });
        button.style().grow = 1.0F;
        button.style().minWidth = 0.0F;
        m_tabs.emplace_back(std::move(name), &button);
    };
    addTab({});
    std::vector<std::string> categories;
    for (const BlockDefinition& block : m_catalog.blocks()) {
        if (!m_icons.iconOf(block.firstState).empty() &&
            std::ranges::find(categories, block.category) == categories.end()) {
            categories.push_back(block.category);
        }
    }
    std::ranges::sort(categories);
    for (std::string& category : categories) {
        addTab(std::move(category));
    }

    auto& scroll = panel.add<ScrollView>();
    scroll.style().grow = 1.0F;
    m_grid = &scroll.add<Widget>();
    m_grid->style().gap = 1.0F;

    panel.add<Label>("Hotbar", TextRole::Muted);
    auto& bar = panel.add<Widget>();
    bar.style().direction = Direction::Row;
    bar.style().gap = 1.0F;
    for (usize slot = 0; slot < m_hotbar.slots.size(); ++slot) {
        auto& button = bar.add<IconButton>(std::string(), [this, slot] {
            m_hotbar.selected = slot;
            syncHotbar();
        });
        button.style().width = kCell;
        button.style().height = kCell;
        button.style().minWidth = 0.0F;
        m_slots.push_back(&button);
    }
    showCategory({});
    syncHotbar();
}

void InventoryScreen::showCategory(std::string category) {
    m_category = std::move(category);
    for (const auto& [name, button] : m_tabs) {
        button->setSelected(name == m_category);
    }
    rebuildGrid();
}

void InventoryScreen::rebuildGrid() {
    using namespace vxe::ui;
    m_grid->clearChildren();
    const std::string needle = lowered(m_search->text());
    Widget* row = nullptr;
    usize used = 0;
    for (const BlockDefinition& block : m_catalog.blocks()) {
        const std::string_view icon = m_icons.iconOf(block.firstState);
        if (icon.empty() || block.hidden || (!m_category.empty() && block.category != m_category) ||
            (!needle.empty() && lowered(block.displayName).find(needle) == std::string::npos)) {
            continue;
        }
        if (used % kColumns == 0) {
            row = &m_grid->add<Widget>();
            row->style().direction = Direction::Row;
            row->style().gap = 1.0F;
        }
        ++used;
        auto& button = row->add<IconButton>(std::string(icon), [this, state = block.firstState] {
            m_hotbar.slots.at(m_hotbar.selected) = state;
            syncHotbar();
        });
        button.style().width = kCell;
        button.style().height = kCell;
        button.style().minWidth = 0.0F;
        button.setTooltip(block.displayName);
    }
    if (used == 0) {
        m_grid->add<Label>("No block matches.", TextRole::Muted);
    }
}

void InventoryScreen::syncHotbar() {
    for (usize slot = 0; slot < m_slots.size(); ++slot) {
        m_slots[slot]->setIcon(std::string(m_icons.iconOf(m_hotbar.slots.at(slot))));
        m_slots[slot]->setSelected(slot == m_hotbar.selected);
    }
}

void InventoryScreen::update(f32 /*deltaSeconds*/) {
    const bool held = m_services.input.isKeyDown(kKeyE);
    if (!held) {
        m_keyReleased = true;
    } else if (m_keyReleased && !m_search->focused()) {
        manager()->pop();
    }
}

} // namespace vr
