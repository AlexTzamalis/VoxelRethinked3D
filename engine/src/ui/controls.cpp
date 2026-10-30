#include "vxe/ui/controls.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace vxe::ui {
namespace {

// The switch of a Toggle and its knob, in UI pixels.
constexpr f32 kSwitchWidth = 22.0F;
constexpr f32 kSwitchHeight = 12.0F;
constexpr f32 kKnobSize = 8.0F;
// The info badge's square.
constexpr f32 kBadgeSize = 9.0F;
// A slider's knob width.
constexpr f32 kSliderKnob = 4.0F;
// Space for the "<" and ">" of a cycle button, on each side.
constexpr f32 kArrowSpace = 10.0F;
// The scroll bar's thumb never gets shorter than this.
constexpr f32 kMinThumb = 12.0F;

// The frame color of a control: the focus color while hovered or focused.
u32 frameColor(const Theme& theme, bool highlighted) noexcept {
    return highlighted ? theme.colors.focus : theme.colors.fieldBorder;
}

// Half transparent accent for filled tracks, so text on them stays readable.
u32 trackFill(const Theme& theme) noexcept {
    return (theme.colors.accent & 0x00FFFFFFU) | (0xA0U << 24U);
}

} // namespace

Toggle::Toggle(bool value, std::function<void(bool)> onChange)
    : m_value(value)
    , m_onChange(std::move(onChange)) {}

void Toggle::setValue(bool value) {
    if (value != m_value) {
        m_value = value;
        invalidateVisual();
    }
}

void Toggle::flip() {
    m_value = !m_value;
    invalidateVisual();
    if (m_onChange) {
        const auto onChange = m_onChange;
        onChange(m_value);
    }
}

Size Toggle::measureContent(const UiEnvironment& /*environment*/, f32 /*availableWidth*/) const {
    return {kSwitchWidth, kSwitchHeight};
}

void Toggle::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    const UiRect track{box.x, box.y + std::floor((box.height - kSwitchHeight) / 2.0F), kSwitchWidth,
                       kSwitchHeight};
    context.fillRect(track, frameColor(theme, m_hovered || m_focused));
    context.fillRect({track.x + 1.0F, track.y + 1.0F, track.width - 2.0F, track.height - 2.0F},
                     m_value ? theme.colors.accent : theme.colors.field);
    const f32 inset = (kSwitchHeight - kKnobSize) / 2.0F;
    const f32 knobX = m_value ? track.x + track.width - inset - kKnobSize : track.x + inset;
    context.fillRect({knobX, track.y + inset, kKnobSize, kKnobSize}, theme.colors.text);
}

void Toggle::onHover(bool hovered) {
    m_hovered = hovered;
    invalidateVisual();
}

void Toggle::onFocus(bool focused) {
    m_focused = focused;
    invalidateVisual();
}

void Toggle::onRelease(bool inside) {
    if (inside) {
        flip();
    }
}

void Toggle::activate() {
    flip();
}

bool Toggle::onAdjust(i32 direction) {
    // Right turns it on, Left off, like the knob moves.
    if ((direction > 0) != m_value) {
        flip();
    }
    return true;
}

Slider::Slider(f64 min, f64 max, f64 step, f64 value, std::function<void(f64)> onChange)
    : m_min(min)
    , m_max(std::max(min, max))
    , m_step(step)
    , m_value(min)
    , m_onChange(std::move(onChange)) {
    setFormat({});
    setValue(value);
}

void Slider::setFormat(std::function<std::string(f64)> format) {
    if (!format) {
        // Whole numbers for whole steps, else as many decimals as the step.
        const f64 step = m_step;
        int decimals = 0;
        if (step > 0.0 && step < 1.0) {
            decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step) - 1e-9)), 1, 3);
        } else if (step <= 0.0 && m_max - m_min <= 10.0) {
            decimals = 2;
        }
        format = [decimals](f64 value) { return std::format("{:.{}f}", value, decimals); };
    }
    m_format = std::move(format);
    m_text = m_format(m_value);
    invalidateVisual();
}

f64 Slider::snap(f64 value) const noexcept {
    value = std::clamp(value, m_min, m_max);
    if (m_step > 0.0) {
        value = m_min + std::round((value - m_min) / m_step) * m_step;
        value = std::clamp(value, m_min, m_max);
    }
    return value;
}

void Slider::setValue(f64 value) {
    value = snap(value);
    if (value != m_value || m_text.empty()) {
        m_value = value;
        m_text = m_format ? m_format(m_value) : std::string();
        invalidateVisual();
    }
}

void Slider::change(f64 value) {
    const f64 before = m_value;
    setValue(value);
    if (m_value != before && m_onChange) {
        const auto onChange = m_onChange;
        onChange(m_value);
    }
}

void Slider::setFromPointer(f32 x) {
    const UiRect& box = rect();
    const f32 usable = std::max(1.0F, box.width - kSliderKnob);
    const f64 fraction = std::clamp(static_cast<f64>((x - box.x - kSliderKnob / 2.0F) / usable), 0.0, 1.0);
    change(m_min + fraction * (m_max - m_min));
}

Size Slider::measureContent(const UiEnvironment& environment, f32 /*availableWidth*/) const {
    return {environment.theme.metrics.buttonMinWidth, environment.theme.metrics.buttonHeight};
}

void Slider::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    context.fillRect(box, frameColor(theme, m_hovered || m_focused || m_dragging));
    const UiRect inner{box.x + 1.0F, box.y + 1.0F, box.width - 2.0F, box.height - 2.0F};
    context.fillRect(inner, theme.colors.field);
    const f64 fraction = m_max > m_min ? (m_value - m_min) / (m_max - m_min) : 0.0;
    const f32 usable = std::max(0.0F, inner.width - kSliderKnob);
    const f32 knobX = inner.x + std::round(static_cast<f32>(fraction) * usable);
    context.fillRect({inner.x, inner.y, knobX - inner.x, inner.height}, trackFill(theme));
    context.fillRect({knobX, inner.y, kSliderKnob, inner.height}, theme.colors.text);
    if (const PixelFont* font = context.font(); font != nullptr) {
        const f32 width = std::ceil(font->measure(m_text).width);
        // Centered, unless the knob would hide it: then in the middle of the
        // wider side next to the knob.
        f32 x = box.x + std::floor((box.width - width) / 2.0F);
        if (x < knobX + kSliderKnob && x + width > knobX) {
            const f32 leftWidth = knobX - inner.x;
            const f32 rightStart = knobX + kSliderKnob;
            const f32 rightWidth = inner.x + inner.width - rightStart;
            x = leftWidth > rightWidth ? inner.x + std::floor((leftWidth - width) / 2.0F)
                                       : rightStart + std::floor((rightWidth - width) / 2.0F);
        }
        context.text(m_text, x, box.y + std::floor((box.height - kFontCellPixels) / 2.0F), theme.colors.text,
                     true);
    }
}

void Slider::onHover(bool hovered) {
    m_hovered = hovered;
    invalidateVisual();
}

void Slider::onFocus(bool focused) {
    m_focused = focused;
    invalidateVisual();
}

void Slider::onPress(f32 x, f32 /*y*/) {
    m_dragging = true;
    setFromPointer(x);
}

void Slider::onDrag(f32 x, f32 /*y*/) {
    if (m_dragging) {
        setFromPointer(x);
    }
}

void Slider::onRelease(bool /*inside*/) {
    m_dragging = false;
    invalidateVisual();
}

bool Slider::onAdjust(i32 direction) {
    // Without a step, a twentieth of the range per press.
    const f64 step = m_step > 0.0 ? m_step : (m_max - m_min) / 20.0;
    change(m_value + step * direction);
    return true;
}

CycleButton::CycleButton(std::vector<std::string> choices, usize index, std::function<void(usize)> onChange)
    : m_choices(std::move(choices))
    , m_index(std::min(index, m_choices.empty() ? usize{0} : m_choices.size() - 1))
    , m_onChange(std::move(onChange)) {}

void CycleButton::setIndex(usize index) {
    if (index < m_choices.size() && (index != m_index || !m_offList.empty())) {
        m_index = index;
        m_offList.clear();
        invalidateLayout(); // The text width changes.
    }
}

void CycleButton::showOffList(std::string text) {
    if (text != m_offList) {
        m_offList = std::move(text);
        invalidateLayout();
    }
}

void CycleButton::step(i32 direction) {
    if (m_choices.size() < 2) {
        return;
    }
    const usize count = m_choices.size();
    m_offList.clear();
    m_index = direction > 0 ? (m_index + 1) % count : (m_index + count - 1) % count;
    invalidateLayout();
    if (m_onChange) {
        const auto onChange = m_onChange;
        onChange(m_index);
    }
}

Size CycleButton::measureContent(const UiEnvironment& environment, f32 /*availableWidth*/) const {
    const Theme::Metrics& metrics = environment.theme.metrics;
    f32 widest = 0.0F;
    if (environment.font != nullptr) {
        for (const std::string& choice : m_choices) {
            widest = std::max(widest, std::ceil(environment.font->measure(choice).width));
        }
        m_textWidth = m_choices.empty() ? 0.0F : std::ceil(environment.font->measure(shownText()).width);
    }
    // As wide as the widest choice, so the button does not jump while cycling.
    return {std::max(metrics.buttonMinWidth, widest + 2.0F * metrics.buttonPadding), metrics.buttonHeight};
}

void CycleButton::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    const bool highlighted = m_hovered || m_focused;
    const bool down = m_pressed && m_hovered;
    std::string_view image = "button";
    if (down) {
        image = "button_pressed";
    } else if (highlighted) {
        image = "button_hover";
    }
    if (!context.nineSlice(box, image, theme.slices.button)) {
        context.fillRect(box, highlighted ? theme.colors.focus : theme.colors.buttonBorder);
        u32 fill = theme.colors.button;
        if (down) {
            fill = theme.colors.buttonPressed;
        } else if (highlighted) {
            fill = theme.colors.buttonHover;
        }
        context.fillRect({box.x + 1.0F, box.y + 1.0F, box.width - 2.0F, box.height - 2.0F}, fill);
    }
    const f32 y = box.y + std::floor((box.height - kFontCellPixels) / 2.0F) + (down ? 1.0F : 0.0F);
    const PixelFont* font = context.font();
    if (!m_choices.empty() && font != nullptr) {
        // Room for the arrows on both sides; longer choices end in "...".
        const std::string text = font->ellipsize(shownText(), box.width - 2.0F * kArrowSpace);
        const f32 width = std::ceil(font->measure(text).width);
        context.text(text, box.x + std::floor((box.width - width) / 2.0F), y, theme.colors.text, true);
    }
    if (highlighted && m_choices.size() > 1) {
        // Arrows hint that Left and Right (or clicks) cycle.
        context.text("<", box.x + 4.0F, y, theme.colors.textMuted, false);
        context.text(">", box.x + box.width - 8.0F, y, theme.colors.textMuted, false);
    }
}

void CycleButton::onHover(bool hovered) {
    m_hovered = hovered;
    invalidateVisual();
}

void CycleButton::onFocus(bool focused) {
    m_focused = focused;
    invalidateVisual();
}

void CycleButton::onPress(f32 /*x*/, f32 /*y*/) {
    m_pressed = true;
    invalidateVisual();
}

void CycleButton::onRelease(bool inside) {
    m_pressed = false;
    invalidateVisual();
    if (inside) {
        step(1);
    }
}

void CycleButton::activate() {
    step(1);
}

bool CycleButton::onAdjust(i32 direction) {
    step(direction);
    return true;
}

InfoBadge::InfoBadge(std::string details) {
    setTooltip(std::move(details));
}

Size InfoBadge::measureContent(const UiEnvironment& /*environment*/, f32 /*availableWidth*/) const {
    return {kBadgeSize, kBadgeSize};
}

void InfoBadge::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    const UiRect badge{box.x, box.y + std::floor((box.height - kBadgeSize) / 2.0F), kBadgeSize, kBadgeSize};
    context.fillRect(badge, theme.colors.fieldBorder);
    context.fillRect({badge.x + 1.0F, badge.y + 1.0F, badge.width - 2.0F, badge.height - 2.0F},
                     theme.colors.field);
    // The "i" of the pixel font is one pixel wide: the middle column.
    context.text("i", badge.x + 4.0F, badge.y + 1.0F, theme.colors.textMuted, false);
}

f32 ScrollView::maxScroll() const noexcept {
    return std::max(0.0F, m_contentHeight - m_viewport.height);
}

void ScrollView::scrollTo(f32 offset) {
    const f32 clamped = std::clamp(std::round(offset), 0.0F, maxScroll());
    if (clamped != m_scroll) {
        m_scroll = clamped;
        invalidateLayout();
    }
}

Size ScrollView::measureChildren(const UiEnvironment& environment, f32 innerWidth) const {
    // As wide as the content, but no height of its own: the parent decides.
    if (children().empty()) {
        return {};
    }
    return {children().front()->measure(environment, innerWidth).width, 0.0F};
}

void ScrollView::arrangeChildren(const UiEnvironment& environment, const UiRect& inner) {
    m_viewport = inner;
    if (children().empty()) {
        m_contentHeight = 0.0F;
        return;
    }
    Widget& content = *children().front();
    f32 width = inner.width;
    Size size = content.measure(environment, width);
    if (size.height > inner.height) {
        // The bar takes room from the content only while it is needed.
        width = std::max(0.0F, inner.width - kBarWidth - kBarGap);
        size = content.measure(environment, width);
    }
    m_contentHeight = size.height;
    m_scroll = std::clamp(m_scroll, 0.0F, maxScroll());
    content.arrange(environment, {inner.x, inner.y - m_scroll, width, std::max(size.height, inner.height)});
    for (usize index = 1; index < children().size(); ++index) {
        children()[index]->arrange(environment, {});
    }
}

void ScrollView::drawChildren(DrawContext& context) const {
    context.pushClip(m_viewport);
    Widget::drawChildren(context);
    context.popClip();
    const f32 range = maxScroll();
    if (range <= 0.0F) {
        return;
    }
    const Theme& theme = context.theme();
    const UiRect track{m_viewport.x + m_viewport.width - kBarWidth, m_viewport.y, kBarWidth,
                       m_viewport.height};
    context.fillRect(track, theme.colors.field);
    const f32 thumb = std::max(kMinThumb, std::floor(track.height * track.height / m_contentHeight));
    const f32 y = track.y + std::round((track.height - thumb) * m_scroll / range);
    context.fillRect({track.x, y, track.width, thumb}, theme.colors.textMuted);
}

void ScrollView::onWheel(f32 lines) {
    scrollTo(m_scroll - lines * kWheelStep);
}

void ScrollView::reveal(const UiRect& rect) {
    if (rect.y < m_viewport.y) {
        scrollTo(m_scroll - (m_viewport.y - rect.y));
    } else if (rect.y + rect.height > m_viewport.y + m_viewport.height) {
        scrollTo(m_scroll + (rect.y + rect.height - m_viewport.y - m_viewport.height));
    }
}

} // namespace vxe::ui
