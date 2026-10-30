#pragma once

#include <vxe/ui/widget.hpp>

namespace vr {

// Debug.Ui.WidgetSample: a small screen with every widget of E12.2 (a
// panel, titles, wrapped text, buttons in a row, a disabled button and a
// click counter), to check the theme, the layout and the pointer by eye.
// Builds into `root`, which it centers.
void buildWidgetSample(vxe::ui::Widget& root);

} // namespace vr
