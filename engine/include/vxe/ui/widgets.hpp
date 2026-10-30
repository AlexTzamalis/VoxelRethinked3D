#pragma once

#include <vxe/ui/widget.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The first widgets of the game UI (E12.2): panels, text and buttons. More
// (sliders, toggles, text fields, lists) follow with the screens that need
// them. Their look comes from the Theme, so packs restyle them.
namespace vxe::ui {

// A box with the theme's panel look (the pack's ui/panel.png as a nine
// slice frame, else the panel colors), padding and gap: the frame of a menu
// or a dialog. It blocks the pointer, so clicks on it never reach the world
// behind.
class Panel : public Widget {
public:
    // A flat color instead of the theme's panel look (a transparent one
    // draws nothing, for invisible layout boxes that still block the pointer).
    void setBackground(std::optional<u32> color) {
        m_background = color;
        invalidateVisual();
    }
    void setBorder(bool border) {
        m_border = border;
        invalidateVisual();
    }

    [[nodiscard]] Edges themePadding(const Theme& theme) const noexcept override;
    [[nodiscard]] f32 themeGap(const Theme& theme) const noexcept override;
    void draw(DrawContext& context) const override;
    [[nodiscard]] bool blocksPointer() const noexcept override { return true; }

private:
    std::optional<u32> m_background;
    bool m_border = true;
};

// Which theme color a label uses.
enum class TextRole : u8 {
    Normal,
    Muted,
    Title
};
enum class TextAlign : u8 {
    Start,
    Center,
    End
};

// One or more lines of text. With wrapping on, the text breaks at spaces to
// fit the width its parent gives it.
class Label : public Widget {
public:
    explicit Label(std::string text = {}, TextRole role = TextRole::Normal);

    void setText(std::string text);
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }
    void setRole(TextRole role) {
        m_role = role;
        invalidateVisual();
    }
    // Overrides the role's color.
    void setColor(std::optional<u32> color) {
        m_color = color;
        invalidateVisual();
    }
    void setAlign(TextAlign align) {
        m_align = align;
        invalidateVisual();
    }
    void setWrap(bool wrap);
    void setShadow(bool shadow) {
        m_shadow = shadow;
        invalidateVisual();
    }

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;

private:
    std::string m_text;
    // The text as drawn: wrapped at the last layout's width.
    mutable std::string m_lines;
    mutable Size m_linesSize;
    TextRole m_role;
    std::optional<u32> m_color;
    TextAlign m_align = TextAlign::Start;
    bool m_wrap = false;
    bool m_shadow = true;
};

// A clickable button with a centered label. Its height and minimum width
// come from the theme; it grows with its label otherwise. Its look is the
// pack's ui/button.png, button_hover.png, button_pressed.png and
// button_disabled.png as nine slice frames, else the theme's button colors. A click is a press
// and a release over the button; disabled buttons ignore the pointer and
// gray out.
class Button : public Widget {
public:
    explicit Button(std::string text = {}, std::function<void()> onClick = {});

    void setText(std::string text);
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }
    void setOnClick(std::function<void()> onClick) { m_onClick = std::move(onClick); }
    void setEnabled(bool enabled) {
        m_enabled = enabled;
        invalidateVisual();
    }
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    [[nodiscard]] bool hovered() const noexcept { return m_hovered; }
    [[nodiscard]] bool pressed() const noexcept { return m_pressed; }

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;
    [[nodiscard]] bool takesPointer() const noexcept override { return m_enabled; }
    [[nodiscard]] bool blocksPointer() const noexcept override { return true; }
    void onHover(bool hovered) override {
        m_hovered = hovered;
        invalidateVisual();
    }
    void onPress() override {
        m_pressed = true;
        invalidateVisual();
    }
    void onRelease(bool inside) override;

private:
    void drawFlat(DrawContext& context) const;

    std::string m_text;
    std::function<void()> m_onClick;
    // The label's width at the last layout, for centering without measuring
    // while drawing.
    mutable f32 m_textWidth = 0.0F;
    bool m_enabled = true;
    bool m_hovered = false;
    bool m_pressed = false;
};

} // namespace vxe::ui
