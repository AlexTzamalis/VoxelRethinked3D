#include "keybinds_screen.hpp"

#include <vxe/ui/keybinds_view.hpp>
#include <vxe/ui/widgets.hpp>

namespace vr {

void KeybindsScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    // The same centered panel as the settings screen.
    root.style().direction = Direction::Row;
    root.style().justify = Justify::Center;
    root.style().padding = Edges::all(8.0F);
    auto& panel = root.add<Panel>();
    panel.style().grow = 1.0F;
    panel.style().maxWidth = 480.0F;
    panel.add<Label>("Controls", TextRole::Title).setAlign(TextAlign::Center);
    panel.add<KeybindsView>(m_services.input, m_services.ui);
    auto& buttons = panel.add<Widget>();
    buttons.style().direction = Direction::Row;
    buttons.style().justify = Justify::End;
    buttons.add<Button>("Done", [this] { manager()->pop(); });
}

} // namespace vr
