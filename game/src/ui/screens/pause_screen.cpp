#include "pause_screen.hpp"

#include "settings_screen.hpp"

#include <vxe/ui/widgets.hpp>

#include <memory>

namespace vr {

void PauseScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    root.style().justify = Justify::Center;
    root.style().align = Align::Center;
    auto& panel = root.add<Panel>();
    panel.style().width = 200.0F;
    panel.add<Label>("Game paused", TextRole::Title).setAlign(TextAlign::Center);
    panel.add<Button>("Back to game", [this] { m_resume(); });
    panel.add<Button>("Settings", [this] { manager()->push(std::make_unique<SettingsScreen>(m_services)); });
    if (m_statistics) {
        panel.add<Button>("Statistics", [this] { m_statistics(); });
    }
    panel.add<Button>("Quit to title", [this] { m_quitToTitle(); });
}

bool PauseScreen::onBack() {
    m_resume();
    return true;
}

} // namespace vr
