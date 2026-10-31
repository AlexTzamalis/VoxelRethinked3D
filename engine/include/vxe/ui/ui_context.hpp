#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/ui/theme.hpp>
#include <vxe/ui/ui_input.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/ui/widget.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vxe::ui {

struct UiContextDesc {
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
};

// How a layer above the root behaves.
struct LayerOptions {
    // A modal layer dims everything below with the theme's backdrop and
    // takes the pointer and the keyboard focus while it is on top.
    bool modal = true;
    // Back (Escape) while the layer is on top; without one, Back closes it.
    std::function<void()> onBack{};
};

// The game UI's root (Section 19.1): owns the widget tree, layers above it
// (dialogs), the theme and the focus; picks the UI scale, lays the trees
// out when they changed, routes the pointer and the keyboard, shows
// tooltips and draws everything with a UiRenderer.
//
// The scale is a whole number: Interface.UiScale, or with 0 (Auto) the
// largest that fits a 320 x 240 UI pixel screen into the window, the
// classic pixel art rule. Whole numbers keep every UI pixel the same size,
// so text and pixel art stay crisp.
//
// Keyboard (E12.4): Tab and Shift+Tab walk the focusable widgets in tree
// order, the arrows move to the nearest one in that direction, Enter
// activates the focused one, Back closes the top layer or is handed to the
// screen (takeBack). Hovering a focusable widget focuses it, unless a text
// field has the focus; clicking focuses what was clicked. The focused text
// field gets the typed text.
//
// Settings: Interface.UiScale (0 Auto, 1 to 8), live.
// Stats: Ui.Widgets (visible widgets), Ui.Scale, Ui.LayoutMs (the last
// layout), Ui.Layouts and Ui.Rebuilds (counters).
//
// Thread safety: main thread only.
class UiContext {
public:
    // The smallest window, in UI pixels, the automatic scale keeps.
    static constexpr u32 kReferenceWidth = 320;
    static constexpr u32 kReferenceHeight = 240;
    static constexpr i32 kMaxScale = 8;
    // How long the pointer rests on a widget before its tooltip shows.
    static constexpr f32 kTooltipDelaySeconds = 0.5F;

    [[nodiscard]] static std::unique_ptr<UiContext> create(const UiContextDesc& desc);

    [[nodiscard]] Widget& root() noexcept { return m_root; }
    void setTheme(Theme theme);
    [[nodiscard]] const Theme& theme() const noexcept { return m_theme; }

    // The largest whole scale at which a window still holds the reference size.
    [[nodiscard]] static u32 autoScale(u32 windowWidth, u32 windowHeight) noexcept;
    [[nodiscard]] u32 scale() const noexcept { return m_scale; }

    // Puts a full window layer above the root and the layers before it (a
    // dialog). The context owns it until closeLayer.
    Widget& pushLayer(std::unique_ptr<Widget> layer, LayerOptions options = {});
    // Closes a layer at the start of the next update, so a button inside it
    // may close it from its click. Focus returns to where it was before.
    void closeLayer(const Widget& layer);
    [[nodiscard]] usize layerCount() const noexcept { return m_layers.size(); }
    // What takes input now: the top layer, or the root without layers.
    [[nodiscard]] Widget& topWidget() noexcept;

    // Lays the trees out if they, the window, the scale or the font changed,
    // then routes the pointer (hover, press, a release over the same widget
    // is a click) and the keyboard, and times tooltips. A click or a key may
    // change the trees; nothing touches the widget afterwards.
    // Runs whenever a widget is pressed, so that a game can give clicks a sound
    // without every widget knowing about sound.
    void setOnPress(std::function<void()> onPress) { m_onPress = std::move(onPress); }

    void update(u32 windowWidth, u32 windowHeight, const UiInput& input, const UiRenderer& renderer);
    // Adds the UI's quads to the renderer's frame. They are kept and rebuilt
    // only when something changed (layout, hover, focus, text, the theme,
    // the scale or the atlas), so a still screen costs one copy per frame.
    void draw(UiRenderer& renderer);
    // How many times draw() rebuilt the kept quads.
    [[nodiscard]] u64 rebuildCount() const noexcept { return m_rebuilds; }

    // True while the pointer is over a widget that blocks it (a panel, a
    // button) or a modal layer is open: the game should not act on clicks.
    [[nodiscard]] bool wantsPointer() const noexcept { return m_overBlocking; }
    // True while a text field has the focus: the game should enable the
    // window's text input and ignore its own keys.
    [[nodiscard]] bool wantsTextInput() const noexcept;
    // True once after Back was pressed with no layer open: the screen decides
    // what Back means (leave the menu, resume the game).
    [[nodiscard]] bool takeBack() noexcept { return std::exchange(m_backRequested, false); }

    [[nodiscard]] const Widget* hovered() const noexcept { return m_hovered; }
    [[nodiscard]] Widget* focused() const noexcept { return m_focused; }
    // Moves the focus; nullptr clears it.
    void setFocus(Widget* widget);
    // The tooltip shown now, or empty.
    [[nodiscard]] std::string_view visibleTooltip() const noexcept;

    // Where a remembered widget is: removed, there but unable to take input
    // (hidden, inert, under a modal layer), or usable.
    enum class Presence : u8 {
        Gone,
        Hidden,
        Live
    };

private:
    struct Layer {
        std::unique_ptr<Widget> root;
        LayerOptions options;
        Widget* focusBefore = nullptr;
        bool closing = false;
    };

    explicit UiContext(const UiContextDesc& desc);
    void closePendingLayers();
    void layout(u32 windowWidth, u32 windowHeight, const UiRenderer& renderer);
    [[nodiscard]] Widget& activeRoot() noexcept;
    void routePointer(const PointerInput& pointer);
    void navigate(const UiInput& input);
    void checkStructure();
    void handleBack();
    [[nodiscard]] bool editText(const TextEditInput& text);
    [[nodiscard]] bool moveFocus(const NavigationInput& keys, const std::vector<Widget*>& focusable,
                                 bool textFocus);
    void updateTooltip(const UiInput& input);
    void drawTooltip(DrawContext& context) const;
    [[nodiscard]] Presence presenceOf(const Widget* widget);
    void dropLostWidgets();

    stats::StatRegistry* m_stats;
    config::Setting<i32> m_scaleSetting;
    Theme m_theme{};
    Widget m_root;
    std::vector<Layer> m_layers;
    u32 m_scale = 1;
    u32 m_width = 0;
    u32 m_height = 0;
    u64 m_fontVersion = 0;
    QuadBatch m_cache;
    u64 m_cacheVersion = 0;
    bool m_cacheValid = false;
    u64 m_rebuilds = 0;
    Widget* m_hovered = nullptr;
    std::function<void()> m_onPress;
    Widget* m_pressed = nullptr;
    Widget* m_focused = nullptr;
    bool m_focusFromPointer = false;
    bool m_wasDown = false;
    bool m_overBlocking = false;
    bool m_backRequested = false;
    f32 m_pointerX = -1.0F;
    f32 m_pointerY = -1.0F;
    // Tooltip: the widget it belongs to, how long it has been waited for and
    // where it shows (UI pixels).
    const Widget* m_tipWidget = nullptr; // Compared only, never used: it may be gone.
    std::string m_tipText;
    f32 m_tipTime = 0.0F;
    f32 m_tipX = 0.0F;
    f32 m_tipY = 0.0F;

    struct StatIds {
        stats::StatId widgets;
        stats::StatId scale;
        stats::StatId layoutMs;
        stats::StatId layouts;
        stats::StatId rebuilds;
    } m_statIds{};
};

} // namespace vxe::ui
