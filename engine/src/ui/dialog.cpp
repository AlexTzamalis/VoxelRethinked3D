#include "vxe/ui/dialog.hpp"

#include <vxe/ui/widgets.hpp>

#include <memory>
#include <utility>

namespace vxe::ui {

Widget& showDialog(UiContext& context, DialogDesc desc) {
    auto layer = std::make_unique<Widget>();
    layer->style().justify = Justify::Center;
    layer->style().align = Align::Center;
    const Widget* layerPointer = layer.get();

    auto& panel = layer->add<Panel>();
    panel.style().width = desc.width;
    if (!desc.title.empty()) {
        panel.add<Label>(std::move(desc.title), TextRole::Title).setAlign(TextAlign::Center);
    }
    if (!desc.message.empty()) {
        auto& message = panel.add<Label>(std::move(desc.message));
        message.setWrap(true);
    }
    auto& row = panel.add<Widget>();
    row.style().direction = Direction::Row;
    row.style().gap = context.theme().metrics.gap;
    row.style().padding = Edges{0.0F, 4.0F, 0.0F, 0.0F};

    Button* cancel = nullptr;
    std::function<void()> cancelAction;
    for (DialogButton& entry : desc.buttons) {
        auto& button = row.add<Button>(std::move(entry.text));
        button.style().grow = 1.0F;
        button.style().minWidth = 0.0F;
        // Close first, then act: the action may open the next dialog.
        button.setOnClick([&context, layerPointer, action = entry.onClick] {
            context.closeLayer(*layerPointer);
            if (action) {
                action();
            }
        });
        if (entry.cancel && cancel == nullptr) {
            cancel = &button;
            cancelAction = entry.onClick;
        }
    }
    LayerOptions options;
    if (cancel != nullptr) {
        options.onBack = [&context, layerPointer, cancelAction] {
            context.closeLayer(*layerPointer);
            if (cancelAction) {
                cancelAction();
            }
        };
    }
    Widget& result = context.pushLayer(std::move(layer), std::move(options));
    if (cancel != nullptr) {
        context.setFocus(cancel);
    }
    return result;
}

} // namespace vxe::ui
