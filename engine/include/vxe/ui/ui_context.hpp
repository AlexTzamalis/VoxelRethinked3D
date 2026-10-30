#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/ui/theme.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widget.hpp>

#include <memory>

namespace vxe::ui {

// The pointer as the window sees it this frame.
struct PointerInput {
    f32 x = 0.0F; // Window pixels from the top left.
    f32 y = 0.0F;
    bool primaryDown = false; // The left mouse button (later a gamepad's confirm).
    bool available = true;    // False while the game captures the mouse for looking around.
};

struct UiContextDesc {
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
};

// The game UI's root (Section 19.1): owns the widget tree and the theme,
// picks the UI scale, lays the tree out when it changed, routes the pointer
// to widgets and draws the tree with a UiRenderer.
//
// The scale is a whole number: Interface.UiScale, or with 0 (Auto) the
// largest that fits a 320 x 240 UI pixel screen into the window, the
// classic pixel art rule. Whole numbers keep every UI pixel the same size,
// so text and pixel art stay crisp.
//
// Settings: Interface.UiScale (0 Auto, 1 to 8), live.
// Stats: Ui.Widgets (visible widgets), Ui.Scale, Ui.LayoutMs (the last
// layout) and Ui.Layouts (counter).
//
// Thread safety: main thread only.
class UiContext {
public:
    // The smallest window, in UI pixels, the automatic scale keeps.
    static constexpr u32 kReferenceWidth = 320;
    static constexpr u32 kReferenceHeight = 240;
    static constexpr i32 kMaxScale = 8;

    [[nodiscard]] static std::unique_ptr<UiContext> create(const UiContextDesc& desc);

    [[nodiscard]] Widget& root() noexcept { return m_root; }
    void setTheme(Theme theme);
    [[nodiscard]] const Theme& theme() const noexcept { return m_theme; }

    // The largest whole scale at which a window still holds the reference size.
    [[nodiscard]] static u32 autoScale(u32 windowWidth, u32 windowHeight) noexcept;
    [[nodiscard]] u32 scale() const noexcept { return m_scale; }

    // Lays the tree out if it, the window, the scale or the font changed,
    // then routes the pointer: hover follows it, a press goes to the widget
    // under it, and a release over the same widget is a click (which may
    // change the tree; nothing touches the clicked widget afterwards).
    void update(u32 windowWidth, u32 windowHeight, const PointerInput& pointer, const UiRenderer& renderer);
    // Adds the tree's rectangles and text to the renderer's frame.
    void draw(UiRenderer& renderer) const;
    // True while the pointer is over a widget that blocks it (a panel, a
    // button): the game should not act on that click itself.
    [[nodiscard]] bool wantsPointer() const noexcept { return m_overBlocking; }
    // The widget under the pointer that takes it (nothing when none).
    [[nodiscard]] const Widget* hovered() const noexcept { return m_hovered; }

private:
    explicit UiContext(const UiContextDesc& desc);
    void layout(u32 windowWidth, u32 windowHeight, const UiRenderer& renderer);
    void routePointer(const PointerInput& pointer);

    stats::StatRegistry* m_stats;
    config::Setting<i32> m_scaleSetting;
    Theme m_theme{};
    Widget m_root;
    u32 m_scale = 1;
    u32 m_width = 0;
    u32 m_height = 0;
    u64 m_fontVersion = 0;
    Widget* m_hovered = nullptr;
    Widget* m_pressed = nullptr;
    bool m_wasDown = false;
    bool m_overBlocking = false;

    struct StatIds {
        stats::StatId widgets;
        stats::StatId scale;
        stats::StatId layoutMs;
        stats::StatId layouts;
    } m_statIds{};
};

} // namespace vxe::ui
