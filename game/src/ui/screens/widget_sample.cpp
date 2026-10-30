#include "widget_sample.hpp"

#include <vxe/ui/dialog.hpp>
#include <vxe/ui/widgets.hpp>

#include <format>
#include <memory>

namespace vr {

void buildWidgetSample(vxe::ui::UiContext& context) {
    using namespace vxe::ui;
    Widget& root = context.root();
    LayoutStyle& rootStyle = root.style();
    rootStyle.justify = Justify::Center;
    rootStyle.align = Align::Center;

    auto& panel = root.add<Panel>();
    panel.style().width = 240.0F;

    auto& title = panel.add<Label>("Voxel Rethinked", TextRole::Title);
    title.setAlign(TextAlign::Center);
    auto& text =
        panel.add<Label>("Widgets of the new interface: panels, labels that wrap to their width, and "
                         "buttons that react to the mouse.",
                         TextRole::Muted);
    text.setWrap(true);
    auto& name = panel.add<TextField>("", "World name");
    name.setMaxLength(32);

    auto& row = panel.add<Widget>();
    row.style().direction = Direction::Row;
    row.style().gap = 4.0F;
    auto& counter = panel.add<Label>("Clicked 0 times");
    counter.setAlign(TextAlign::Center);
    auto& clickMe = row.add<Button>("Click me");
    clickMe.style().grow = 1.0F;
    // Shared by both buttons; the widgets live as long as the tree.
    auto clicks = std::make_shared<int>(0);
    clickMe.setOnClick([&counter, clicks] {
        ++*clicks;
        counter.setText(std::format("Clicked {} {}", *clicks, *clicks == 1 ? "time" : "times"));
    });
    auto& reset = row.add<Button>("Reset");
    reset.style().grow = 1.0F;
    reset.setOnClick([&counter, clicks] {
        *clicks = 0;
        counter.setText("Clicked 0 times");
    });

    auto& multiplayer = panel.add<Button>("Multiplayer");
    multiplayer.setEnabled(false);
    multiplayer.setTooltip("Coming later. Worlds are singleplayer for now.");
    auto& remove = panel.add<Button>("Delete world");
    remove.setOnClick([&context, &name, &counter] {
        const std::string world = name.text().empty() ? std::string("this world") : name.text();
        showDialog(context,
                   {.title = "Delete world?",
                    .message = std::format("{} will be gone for good.", world),
                    .buttons = {{.text = "Delete", .onClick = [&counter] { counter.setText("Deleted"); }},
                                {.text = "Cancel", .cancel = true}}});
    });
}

} // namespace vr
