#include "settings_screen.hpp"

#include "keybinds_screen.hpp"

#include <vxe/ui/dialog.hpp>
#include <vxe/ui/widgets.hpp>

#include <memory>

namespace vr {

void SettingsScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    // A row that centers one panel: the panel grows up to its maximum width,
    // and the free space left over centers it.
    root.style().direction = Direction::Row;
    root.style().justify = Justify::Center;
    root.style().padding = Edges::all(8.0F);
    auto& panel = root.add<Panel>();
    panel.style().grow = 1.0F;
    panel.style().maxWidth = 600.0F; // The group tabs need the width.
    panel.add<Label>("Settings", TextRole::Title).setAlign(TextAlign::Center);
#ifdef NDEBUG
    constexpr bool kDeveloper = false;
#else
    constexpr bool kDeveloper = true;
#endif
    m_view = &panel.add<SettingsView>(m_services.settings,
                                      SettingsViewOptions{.showDeveloper = kDeveloper, .searchBox = true});
    if (!m_firstGroup.empty()) {
        m_view->showGroup(m_firstGroup);
    }
    if (!m_search.empty()) {
        m_view->searchBox()->setText(m_search);
        m_view->setFilter(m_search);
    }
    auto& buttons = panel.add<Widget>();
    buttons.style().direction = Direction::Row;
    buttons.style().justify = Justify::SpaceBetween;
    buttons.add<Button>("Keybinds",
                        [this] { manager()->push(std::make_unique<KeybindsScreen>(m_services)); });
    m_resetPage = &buttons.add<Button>("Reset page", [this] {
        showDialog(m_services.ui, {.title = "Reset these settings?",
                                   .message = "Every setting shown goes back to its default value.",
                                   .buttons = {{.text = "Reset", .onClick = [this] { m_view->resetPage(); }},
                                               {.text = "Cancel", .cancel = true}}});
    });
    buttons.add<Button>("Done", [this] { manager()->pop(); });
}

void SettingsScreen::update(vxe::f32 /*deltaSeconds*/) {
    if (m_view != nullptr) {
        m_view->refresh();
        m_resetPage->setEnabled(m_view->pageChanged());
    }
}

} // namespace vr
