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
        if (enabled != m_enabled) {
            m_enabled = enabled;
            invalidateVisual();
        }
    }
    // Shows the button highlighted while selected (the chosen tab or group).
    void setSelected(bool selected) {
        if (selected != m_selected) {
            m_selected = selected;
            invalidateVisual();
        }
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
    void onPress(f32 /*x*/, f32 /*y*/) override {
        m_pressed = true;
        invalidateVisual();
    }
    void onRelease(bool inside) override;
    [[nodiscard]] bool focusable() const noexcept override { return m_enabled; }
    void onFocus(bool focused) override {
        m_focused = focused;
        invalidateVisual();
    }
    // Enter clicks the focused button.
    void activate() override;
    [[nodiscard]] bool focused() const noexcept { return m_focused; }

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
    bool m_focused = false;
    bool m_selected = false;
};

// A one line text input (world names, seeds): click or Tab to focus, type,
// Backspace and Delete erase, Left, Right, Home and End move the caret,
// Enter submits. Text is UTF-8; the caret always sits between code points.
// Long text scrolls so the caret stays visible.
class TextField : public Widget {
public:
    explicit TextField(std::string text = {}, std::string placeholder = {});

    void setText(std::string text);
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }
    // Shown in the muted color while the field is empty.
    void setPlaceholder(std::string placeholder);
    // The most code points the field takes; 0 for no limit.
    void setMaxLength(usize codePoints) { m_maxLength = codePoints; }
    // Which characters the field accepts (for example digits only).
    void setFilter(std::function<bool(char32_t)> accepts) { m_accepts = std::move(accepts); }
    void setOnChange(std::function<void(const std::string&)> onChange) { m_onChange = std::move(onChange); }
    void setOnSubmit(std::function<void(const std::string&)> onSubmit) { m_onSubmit = std::move(onSubmit); }
    // The caret as a byte offset into text().
    [[nodiscard]] usize caret() const noexcept { return m_caret; }
    [[nodiscard]] bool focused() const noexcept { return m_focused; }

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;
    [[nodiscard]] bool takesPointer() const noexcept override { return true; }
    [[nodiscard]] bool focusable() const noexcept override { return true; }
    [[nodiscard]] bool capturesText() const noexcept override { return true; }
    void onPress(f32 x, f32 y) override;
    void onFocus(bool focused) override;
    void activate() override;
    void onTextInput(const TextEditInput& text) override;
    void onFocusedFrame(f32 deltaSeconds) override;

private:
    void changed();
    [[nodiscard]] f32 caretOffset(usize bytes) const;

    std::string m_text;
    std::string m_placeholder;
    usize m_caret = 0;
    usize m_maxLength = 0;
    std::function<bool(char32_t)> m_accepts;
    std::function<void(const std::string&)> m_onChange;
    std::function<void(const std::string&)> m_onSubmit;
    // The font of the last layout, to place the caret from a click.
    mutable const PixelFont* m_font = nullptr;
    // How far the text is scrolled left so the caret stays inside.
    mutable f32 m_scroll = 0.0F;
    f32 m_blink = 0.0F;
    bool m_focused = false;
};

} // namespace vxe::ui
