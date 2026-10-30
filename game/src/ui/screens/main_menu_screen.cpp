#include "main_menu_screen.hpp"

#include "settings_screen.hpp"

#include <vxe/core/version.hpp>
#include <vxe/ui/widgets.hpp>

#include <format>
#include <memory>

namespace vr {
namespace {

// The title's font pixels per UI pixel, and the width of the button column.
constexpr vxe::u32 kTitleScale = 4;
constexpr vxe::f32 kMenuWidth = 200.0F;

} // namespace

void MainMenuScreen::build(vxe::ui::Widget& root) {
    using namespace vxe::ui;
    // Two overlapping layers: the centered menu and the version in a corner.
    root.style().direction = Direction::Stack;

    auto& menu = root.add<Widget>();
    menu.style().justify = Justify::Center;
    menu.style().align = Align::Center;
    menu.style().gap = 4.0F;
    auto& title = menu.add<Label>("Voxel Rethinked", TextRole::Title);
    title.setTextScale(kTitleScale);
    title.style().padding = Edges{0.0F, 0.0F, 0.0F, 16.0F};

    const auto addButton = [&menu](std::string text, std::function<void()> onClick) -> Button& {
        auto& button = menu.add<Button>(std::move(text), std::move(onClick));
        button.style().width = kMenuWidth;
        return button;
    };
    addButton("Singleplayer", [this] { m_singleplayer(); });
    auto& multiplayer = addButton("Multiplayer", {});
    multiplayer.setEnabled(false);
    multiplayer.setTooltip("Coming later.");
    addButton("Settings", [this] { manager()->push(std::make_unique<SettingsScreen>(m_services)); });
    auto& packs = addButton("Resource Packs", {});
    packs.setEnabled(false);
    packs.setTooltip("Coming soon. Until then, the Graphics settings list the enabled packs.");
    addButton("Quit", [this] { m_quit(); });

    auto& corner = root.add<Widget>();
    corner.style().justify = Justify::End;
    corner.style().align = Align::Start;
    corner.style().padding = Edges::all(4.0F);
    corner.add<Label>(std::format("Version {}", vxe::engineVersion()), TextRole::Muted);
}

} // namespace vr
