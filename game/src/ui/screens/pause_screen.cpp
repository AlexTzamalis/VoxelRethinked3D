#include "pause_screen.hpp"

#include <vxe/ui/widgets.hpp>

namespace vr {

void PauseScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    root.style().justify = Justify::Center;
    root.style().align = Align::Center;
    auto& panel = root.add<Panel>();
    panel.style().width = 200.0F;
    panel.add<Label>("Game paused", TextRole::Title).setAlign(TextAlign::Center);
    panel.add<Button>("Back to game", [this] { m_resume(); });
    auto& settings = panel.add<Button>("Settings");
    settings.setEnabled(false);
    settings.setTooltip("The settings screen comes next. Until then, the console (`) changes every setting.");
    panel.add<Button>("Quit game", [this] { m_quit(); });
}

bool PauseScreen::onBack() {
    m_resume();
    return true;
}

} // namespace vr
