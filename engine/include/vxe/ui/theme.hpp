#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/ui/color.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace vxe::ui {

// The look of the game UI (Section 19.1): colors and sizes every widget
// reads, so a texture pack restyles all menus with one file. Sizes are in UI
// pixels, which the UI scale multiplies (a font pixel is one UI pixel).
// Colors are RGBA with red in the lowest byte (ui::rgba).
//
// The default theme is textures/default/ui/theme.toml; packs override it
// with their own ui/theme.toml. A theme file only needs the values it
// changes: everything else keeps the built in default below.
struct Theme {
    struct Colors {
        u32 text = rgba(240, 240, 240);
        u32 textMuted = rgba(168, 168, 168);
        u32 textDisabled = rgba(112, 112, 112);
        u32 title = rgba(255, 255, 255);
        u32 panel = rgba(24, 26, 32, 216);
        u32 panelBorder = rgba(8, 8, 10);
        u32 button = rgba(74, 74, 82);
        u32 buttonHover = rgba(94, 100, 112);
        u32 buttonPressed = rgba(53, 54, 60);
        u32 buttonDisabled = rgba(44, 44, 48);
        u32 buttonBorder = rgba(12, 12, 14);
        u32 buttonHighlight = rgba(255, 255, 255, 64); // A one pixel bevel along the top.
        u32 buttonShadow = rgba(0, 0, 0, 96);          // And along the bottom.
        u32 focus = rgba(232, 200, 96);                // The border of a hovered or focused widget.
        u32 backdrop = rgba(0, 0, 0, 144);             // Darkens what is behind a screen.
    } colors;

    struct Metrics {
        f32 buttonHeight = 20.0F;
        f32 buttonMinWidth = 80.0F;
        f32 buttonPadding = 8.0F; // Left and right of the label.
        f32 panelPadding = 8.0F;
        f32 gap = 4.0F; // Between widgets of a layout.
        f32 border = 1.0F;
    } metrics;

    // Reads a theme from TOML text: tables [colors] and [metrics] with the
    // keys of the structs above in snake case ("button_hover"). Colors are
    // "#RRGGBB" or "#RRGGBBAA" strings. Unknown keys and wrong values are
    // reported in `warnings` and skipped, so a pack's typo never breaks the
    // menus. Fails only on malformed TOML (ParseError).
    [[nodiscard]] static Result<Theme> parse(std::string_view text, std::vector<std::string>& warnings);
};

// Parses "#RRGGBB" or "#RRGGBBAA" into a ui::rgba value.
[[nodiscard]] bool parseColor(std::string_view text, u32& out) noexcept;

} // namespace vxe::ui
