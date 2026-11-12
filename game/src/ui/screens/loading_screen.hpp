#pragma once

#include <vxe/ui/screen.hpp>
#include <vxe/ui/widget.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace vr {

// The loading screen: a dark tiled background (ui/loading_background.png, or
// the pack's dirt texture), the title, what is being done, a progress bar and a
// tip. It hides the world while it builds. The bar only moves forward and eases
// towards the estimate, which is rough: chunks around the spawn and far terrain.
class LoadingWidget final : public vxe::ui::Widget {
public:
    explicit LoadingWidget(std::string tip)
        : m_tip(std::move(tip)) {}
    // Brings the bar and the stage up to date; redraws only after a change.
    void sync(vxe::f32 fraction, std::string_view stage, vxe::f32 deltaSeconds);
    void draw(vxe::ui::DrawContext& context) const override;

private:
    vxe::f32 m_shown = 0.0F; // The bar as drawn, 0 to 1.
    std::string m_stage;
    std::string m_tip;
};

// One of the loading tips, picked at random.
[[nodiscard]] std::string_view randomLoadingTip();

// Section 19.3's Loading screen. Cancel goes back to the title (G5.3).
class LoadingScreen final : public vxe::ui::Screen {
public:
    explicit LoadingScreen(std::function<void()> cancel = {})
        : m_cancel(std::move(cancel)) {}
    [[nodiscard]] vxe::ui::ScreenOptions options() const override { return {.backdrop = false}; }
    void build(vxe::ui::Widget& root) override;
    [[nodiscard]] LoadingWidget* loading() const noexcept { return m_loading; }

private:
    std::function<void()> m_cancel;
    LoadingWidget* m_loading = nullptr;
};

} // namespace vr
