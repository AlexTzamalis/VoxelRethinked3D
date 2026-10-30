#pragma once

#include <vxe/ui/ui_context.hpp>

namespace vr {

// Debug.Ui.WidgetSample: a small screen with every widget so far (a panel,
// titles, wrapped text, a text field, buttons in a row, a disabled button
// with a tooltip, a confirmation dialog and a click counter), to check the
// theme, the layout, the pointer and the keyboard by eye. Builds into the
// context's root, which it centers.
void buildWidgetSample(vxe::ui::UiContext& context);

} // namespace vr
