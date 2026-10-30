#pragma once

#include <vxe/ui/widget.hpp>

#include <functional>
#include <string>
#include <vector>

// Value controls of the game UI (E12.6): a switch, a slider, a choice that
// cycles, a scrolling container and an info badge. They are what generated
// settings pages are made of, and every one works with the mouse and the
// keyboard (focus with Tab or the arrows, change with Left, Right and Enter).
namespace vxe::ui {

// An on and off switch: a track that turns the theme's accent color when on
// and a knob. Click, Enter or Left and Right flip it.
class Toggle : public Widget {
public:
    explicit Toggle(bool value = false, std::function<void(bool)> onChange = {});

    // Sets the value without calling the change handler (syncing to a setting).
    void setValue(bool value);
    [[nodiscard]] bool value() const noexcept { return m_value; }
    void setOnChange(std::function<void(bool)> onChange) { m_onChange = std::move(onChange); }

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;
    [[nodiscard]] bool takesPointer() const noexcept override { return true; }
    [[nodiscard]] bool focusable() const noexcept override { return true; }
    void onHover(bool hovered) override;
    void onFocus(bool focused) override;
    void onRelease(bool inside) override;
    void activate() override;
    bool onAdjust(i32 direction) override;

private:
    void flip();

    bool m_value;
    std::function<void(bool)> m_onChange;
    bool m_hovered = false;
    bool m_focused = false;
};

// A horizontal slider over a range with a step; the value is written on it.
// Drag it, click where it should be, or step it with Left and Right.
class Slider : public Widget {
public:
    Slider(f64 min, f64 max, f64 step, f64 value, std::function<void(f64)> onChange = {});

    // Sets the value (snapped and clamped) without calling the change handler.
    void setValue(f64 value);
    [[nodiscard]] f64 value() const noexcept { return m_value; }
    void setOnChange(std::function<void(f64)> onChange) { m_onChange = std::move(onChange); }
    // How the value is written (the default shows whole numbers, or as many
    // decimals as the step has).
    void setFormat(std::function<std::string(f64)> format);
    [[nodiscard]] bool dragging() const noexcept { return m_dragging; }

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;
    [[nodiscard]] bool takesPointer() const noexcept override { return true; }
    [[nodiscard]] bool focusable() const noexcept override { return true; }
    void onHover(bool hovered) override;
    void onFocus(bool focused) override;
    void onPress(f32 x, f32 y) override;
    void onDrag(f32 x, f32 y) override;
    void onRelease(bool inside) override;
    bool onAdjust(i32 direction) override;

private:
    [[nodiscard]] f64 snap(f64 value) const noexcept;
    void change(f64 value);
    void setFromPointer(f32 x);

    f64 m_min;
    f64 m_max;
    f64 m_step;
    f64 m_value;
    std::function<void(f64)> m_onChange;
    std::function<std::string(f64)> m_format;
    std::string m_text;
    bool m_hovered = false;
    bool m_focused = false;
    bool m_dragging = false;
};

// A button that cycles through choices (an enum setting): a click or Enter
// goes to the next one, Left and Right go back and forth.
class CycleButton : public Widget {
public:
    CycleButton(std::vector<std::string> choices, usize index, std::function<void(usize)> onChange = {});

    // Sets the choice without calling the change handler.
    void setIndex(usize index);
    [[nodiscard]] usize index() const noexcept { return m_index; }
    void setOnChange(std::function<void(usize)> onChange) { m_onChange = std::move(onChange); }

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;
    [[nodiscard]] bool takesPointer() const noexcept override { return true; }
    [[nodiscard]] bool focusable() const noexcept override { return true; }
    void onHover(bool hovered) override;
    void onFocus(bool focused) override;
    void onPress(f32 x, f32 y) override;
    void onRelease(bool inside) override;
    void activate() override;
    bool onAdjust(i32 direction) override;

private:
    void step(i32 direction);

    std::vector<std::string> m_choices;
    usize m_index;
    std::function<void(usize)> m_onChange;
    mutable f32 m_textWidth = 0.0F;
    bool m_hovered = false;
    bool m_focused = false;
    bool m_pressed = false;
};

// A small "i" whose tooltip holds the details, so rows stay short.
class InfoBadge : public Widget {
public:
    explicit InfoBadge(std::string details);

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;
};

// Shows one content widget (usually a column) taller than itself: clips it,
// scrolls it with the mouse wheel, keeps the keyboard focus in view and
// draws a scroll bar while the content does not fit. Give it a size or grow
// in its parent; it does not grow with its content.
class ScrollView : public Widget {
public:
    // The content: the first child. Add it with add<...>() like any child.
    [[nodiscard]] f32 scrollOffset() const noexcept { return m_scroll; }
    [[nodiscard]] f32 maxScroll() const noexcept;
    void scrollTo(f32 offset);

    [[nodiscard]] Size measureChildren(const UiEnvironment& environment, f32 innerWidth) const override;
    void arrangeChildren(const UiEnvironment& environment, const UiRect& inner) override;
    void drawChildren(DrawContext& context) const override;
    [[nodiscard]] bool takesWheel() const noexcept override { return true; }
    void onWheel(f32 lines) override;
    void reveal(const UiRect& rect) override;

    // Width of the scroll bar and its gap to the content, in UI pixels.
    static constexpr f32 kBarWidth = 4.0F;
    static constexpr f32 kBarGap = 3.0F;
    // UI pixels per wheel line.
    static constexpr f32 kWheelStep = 24.0F;

private:
    f32 m_scroll = 0.0F;
    f32 m_contentHeight = 0.0F;
    UiRect m_viewport{};
};

} // namespace vxe::ui
