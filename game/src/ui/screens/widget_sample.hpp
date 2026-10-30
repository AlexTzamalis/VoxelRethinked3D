#pragma once

#include <vxe/ui/screen.hpp>
#include <vxe/ui/ui_context.hpp>

namespace vr {

// Debug.Ui.WidgetSample: a small screen with every widget so far (a panel,
// titles, wrapped text, a text field, buttons in a row, a disabled button
// with a tooltip, a confirmation dialog and a click counter), to check the
// theme, the layout, the pointer and the keyboard by eye.
class WidgetSampleScreen final : public vxe::ui::Screen {
public:
    explicit WidgetSampleScreen(vxe::ui::UiContext& context)
        : m_context(context) {}
    void build(vxe::ui::Widget& root) override;

private:
    vxe::ui::UiContext& m_context;
};

} // namespace vr
