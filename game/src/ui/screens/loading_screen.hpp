#pragma once

#include <vxe/ui/screen.hpp>
#include <vxe/ui/widgets.hpp>

namespace vr {

// Shown while a world loads, so entering one never shows an empty sky. The
// full loading screen with progress and tips comes with G5.3.
class LoadingScreen final : public vxe::ui::Screen {
public:
    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = true}; }
    void build(vxe::ui::Widget& root) override {
        using namespace vxe::ui;
        root.style().justify = Justify::Center;
        root.style().align = Align::Center;
        root.add<Label>("Loading world", TextRole::Title).setTextScale(2);
    }
};

} // namespace vr
