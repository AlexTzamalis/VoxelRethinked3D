#include "vxe/ui/widgets.hpp"

#include <vxe/ui/ui_input.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace vxe::ui {

Edges Panel::themePadding(const Theme& theme) const noexcept {
    return Edges::all(theme.metrics.panelPadding);
}

f32 Panel::themeGap(const Theme& theme) const noexcept {
    return theme.metrics.gap;
}

void Panel::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    if (!m_background && m_border && context.nineSlice(rect(), "panel", theme.slices.panel)) {
        return;
    }
    context.fillRect(rect(), m_background.value_or(theme.colors.panel));
    if (m_border && theme.metrics.border > 0.0F) {
        context.frameRect(rect(), theme.colors.panelBorder, theme.metrics.border);
    }
}

Label::Label(std::string text, TextRole role)
    : m_text(std::move(text))
    , m_role(role) {}

void Label::setText(std::string text) {
    if (text != m_text) {
        m_text = std::move(text);
        invalidateLayout();
    }
}

void Label::setWrap(bool wrap) {
    if (wrap != m_wrap) {
        m_wrap = wrap;
        invalidateLayout();
    }
}

void Label::setTextScale(u32 scale) {
    scale = std::max(scale, 1U);
    if (scale != m_textScale) {
        m_textScale = scale;
        invalidateLayout();
    }
}

// Measuring also fixes the lines the label draws: the layout measures every
// widget at the width it will get, the last measure before drawing wins.
Size Label::measureContent(const UiEnvironment& environment, f32 availableWidth) const {
    if (environment.font == nullptr || m_text.empty()) {
        m_lines.clear();
        m_linesSize = {};
        return {};
    }
    const auto scale = static_cast<f32>(m_textScale);
    m_lines = m_wrap && availableWidth < kUnbounded ? environment.font->wrap(m_text, availableWidth / scale)
                                                    : m_text;
    const TextSize size = environment.font->measure(m_lines);
    m_linesSize = {std::ceil(size.width) * scale, std::ceil(size.height) * scale};
    return m_linesSize;
}

void Label::draw(DrawContext& context) const {
    if (m_lines.empty()) {
        return;
    }
    const Theme::Colors& colors = context.theme().colors;
    u32 color = colors.text;
    if (m_role == TextRole::Muted) {
        color = colors.textMuted;
    } else if (m_role == TextRole::Title) {
        color = colors.title;
    }
    color = m_color.value_or(color);
    const UiRect& box = rect();
    // Alignment is per label, not per line: wrapped text keeps a straight
    // left edge, which reads better than centered ragged lines.
    f32 x = box.x;
    if (m_align != TextAlign::Start) {
        const f32 width = std::min(box.width, m_linesSize.width);
        x += m_align == TextAlign::Center ? std::floor((box.width - width) / 2.0F) : box.width - width;
    }
    // Centered vertically, so a label lines up with the buttons of its row.
    const f32 y = box.y + std::max(0.0F, std::floor((box.height - m_linesSize.height) / 2.0F));
    const PixelFont* font = context.font();
    if (!m_wrap && font != nullptr && m_linesSize.width > box.width) {
        // Squeezed by its row: cut with "..." instead of running into neighbors.
        context.text(font->ellipsize(m_lines, box.width / static_cast<f32>(m_textScale)), x, y, color,
                     m_shadow, m_textScale);
        return;
    }
    context.text(m_lines, x, y, color, m_shadow, m_textScale);
}

Button::Button(std::string text, std::function<void()> onClick)
    : m_text(std::move(text))
    , m_onClick(std::move(onClick)) {}

void Button::setText(std::string text) {
    if (text != m_text) {
        m_text = std::move(text);
        invalidateLayout();
    }
}

Size Button::measureContent(const UiEnvironment& environment, f32 /*availableWidth*/) const {
    const Theme::Metrics& metrics = environment.theme.metrics;
    m_textWidth = environment.font != nullptr ? std::ceil(environment.font->measure(m_text).width) : 0.0F;
    return {std::max(metrics.buttonMinWidth, m_textWidth + 2.0F * metrics.buttonPadding),
            metrics.buttonHeight};
}

void Button::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    const bool down = m_enabled && m_pressed && m_hovered;
    std::string_view image = "button";
    if (!m_enabled) {
        image = "button_disabled";
    } else if (down) {
        image = "button_pressed";
    } else if (m_hovered || m_focused || m_selected) {
        image = "button_hover";
    }
    if (!context.nineSlice(box, image, theme.slices.button)) {
        drawFlat(context);
    }
    const f32 x = box.x + std::floor((box.width - m_textWidth) / 2.0F);
    const f32 y = box.y + std::floor((box.height - kFontCellPixels) / 2.0F) + (down ? 1.0F : 0.0F);
    context.text(m_text, x, y, m_enabled ? theme.colors.text : theme.colors.textDisabled, m_enabled);
}

// The look without pack images: a frame, a face by state and a one pixel
// bevel.
void Button::drawFlat(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    const f32 border = theme.metrics.border;
    u32 fill = theme.colors.button;
    if (!m_enabled) {
        fill = theme.colors.buttonDisabled;
    } else if (m_pressed && m_hovered) {
        fill = theme.colors.buttonPressed;
    } else if (m_hovered || m_focused || m_selected) {
        fill = theme.colors.buttonHover;
    }
    context.fillRect(box, theme.colors.buttonBorder);
    const UiRect face{box.x + border, box.y + border, box.width - 2.0F * border, box.height - 2.0F * border};
    context.fillRect(face, fill);
    const bool down = m_enabled && m_pressed && m_hovered;
    if (m_enabled) {
        // A one pixel bevel: light on top, dark at the bottom, swapped while
        // pressed so the button reads as pushed in.
        context.fillRect({face.x, face.y, face.width, 1.0F},
                         down ? theme.colors.buttonShadow : theme.colors.buttonHighlight);
        context.fillRect({face.x, face.y + face.height - 1.0F, face.width, 1.0F},
                         down ? theme.colors.buttonHighlight : theme.colors.buttonShadow);
    }
    if ((m_hovered || m_focused || m_selected) && m_enabled) {
        context.frameRect(box, theme.colors.focus, border);
    }
}

void Button::onRelease(bool inside) {
    m_pressed = false;
    invalidateVisual();
    if (inside && m_enabled && m_onClick) {
        // A copy: the click may remove this button (closing its screen).
        const std::function<void()> click = m_onClick;
        click();
    }
}

void Button::activate() {
    if (m_enabled && m_onClick) {
        const std::function<void()> click = m_onClick;
        click();
    }
}

namespace {

// Text fields: the caret blinks with this period while focused.
constexpr f32 kBlinkSeconds = 0.5F;
// Space between the field's frame and its text, in UI pixels.
constexpr f32 kFieldInset = 4.0F;

// The byte offset of the code point before `bytes` (UTF-8 continuation
// bytes are 10xxxxxx).
usize previousBoundary(std::string_view text, usize bytes) noexcept {
    if (bytes == 0) {
        return 0;
    }
    usize position = bytes - 1;
    while (position > 0 && (static_cast<u8>(text[position]) & 0xC0U) == 0x80U) {
        --position;
    }
    return position;
}

usize nextBoundary(std::string_view text, usize bytes) noexcept {
    if (bytes >= text.size()) {
        return text.size();
    }
    usize position = bytes;
    static_cast<void>(nextCodePoint(text, position));
    return position;
}

usize countCodePoints(std::string_view text) noexcept {
    usize count = 0;
    for (usize position = 0; position < text.size(); ++count) {
        static_cast<void>(nextCodePoint(text, position));
    }
    return count;
}

} // namespace

TextField::TextField(std::string text, std::string placeholder)
    : m_text(std::move(text))
    , m_placeholder(std::move(placeholder))
    , m_caret(m_text.size()) {}

void TextField::setText(std::string text) {
    if (text != m_text) {
        m_text = std::move(text);
        m_caret = m_text.size();
        invalidateVisual();
    }
}

void TextField::setPlaceholder(std::string placeholder) {
    m_placeholder = std::move(placeholder);
    invalidateVisual();
}

void TextField::changed() {
    m_blink = 0.0F; // The caret shows right away after every edit.
    invalidateVisual();
    if (m_onChange) {
        const auto onChange = m_onChange;
        onChange(m_text);
    }
}

Size TextField::measureContent(const UiEnvironment& environment, f32 /*availableWidth*/) const {
    m_font = environment.font;
    return {environment.theme.metrics.buttonMinWidth, environment.theme.metrics.buttonHeight};
}

// Where the caret sits for a byte offset, in UI pixels from the text start:
// the advance of everything before it (one pixel after the last glyph).
f32 TextField::caretOffset(usize bytes) const {
    if (m_font == nullptr || bytes == 0) {
        return 0.0F;
    }
    return m_font->measure(std::string_view(m_text).substr(0, bytes)).width + 1.0F;
}

void TextField::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    context.fillRect(box, theme.colors.fieldBorder);
    const f32 border = theme.metrics.border;
    context.fillRect({box.x + border, box.y + border, box.width - 2.0F * border, box.height - 2.0F * border},
                     theme.colors.field);
    if (m_focused) {
        context.frameRect(box, theme.colors.focus, border);
    }
    const UiRect inner{box.x + kFieldInset, box.y, std::max(0.0F, box.width - 2.0F * kFieldInset),
                       box.height};
    const f32 y = box.y + std::floor((box.height - kFontCellPixels) / 2.0F);
    // Scroll so the caret stays inside: only as far as needed, and back when
    // the text gets shorter.
    const f32 caretX = caretOffset(m_caret);
    if (caretX - m_scroll > inner.width - 1.0F) {
        m_scroll = caretX - inner.width + 1.0F;
    } else if (caretX < m_scroll) {
        m_scroll = caretX;
    }
    m_scroll = std::max(0.0F, m_scroll);
    context.pushClip(inner);
    if (m_text.empty() && !m_focused) {
        context.text(m_placeholder, inner.x, y, theme.colors.textMuted, false);
    } else {
        context.text(m_text, inner.x - m_scroll, y, theme.colors.text, true);
    }
    if (m_focused && m_blink < kBlinkSeconds) {
        context.fillRect({inner.x + caretX - m_scroll, y - 1.0F, 1.0F, kFontCellPixels + 2.0F},
                         theme.colors.text);
    }
    context.popClip();
}

void TextField::onPress(f32 x, f32 /*y*/) {
    // The caret goes to the code point boundary nearest the click.
    const f32 target = x - rect().x - kFieldInset + m_scroll;
    usize best = 0;
    f32 bestDistance = std::abs(target);
    for (usize position = 0; position < m_text.size();) {
        position = nextBoundary(m_text, position);
        const f32 distance = std::abs(caretOffset(position) - target);
        if (distance < bestDistance) {
            best = position;
            bestDistance = distance;
        }
    }
    m_caret = best;
    m_blink = 0.0F;
    invalidateVisual();
}

void TextField::onFocus(bool focused) {
    m_focused = focused;
    m_blink = 0.0F;
    invalidateVisual();
}

void TextField::activate() {
    if (m_onSubmit) {
        const auto onSubmit = m_onSubmit;
        onSubmit(m_text);
    }
}

void TextField::onTextInput(const TextEditInput& text) {
    bool edited = false;
    bool moved = false;
    if (!text.typed.empty()) {
        usize count = countCodePoints(m_text);
        for (usize position = 0; position < text.typed.size();) {
            const usize begin = position;
            const char32_t codePoint = nextCodePoint(text.typed, position);
            const bool control = codePoint < 0x20 || codePoint == 0x7F;
            if (control || (m_accepts && !m_accepts(codePoint)) ||
                (m_maxLength != 0 && count >= m_maxLength)) {
                continue;
            }
            m_text.insert(m_caret, text.typed.substr(begin, position - begin));
            m_caret += position - begin;
            ++count;
            edited = true;
        }
    }
    if (text.erase && m_caret > 0) {
        const usize start = previousBoundary(m_text, m_caret);
        m_text.erase(start, m_caret - start);
        m_caret = start;
        edited = true;
    }
    if (text.eraseNext && m_caret < m_text.size()) {
        m_text.erase(m_caret, nextBoundary(m_text, m_caret) - m_caret);
        edited = true;
    }
    if (text.caretLeft && m_caret > 0) {
        m_caret = previousBoundary(m_text, m_caret);
        moved = true;
    }
    if (text.caretRight && m_caret < m_text.size()) {
        m_caret = nextBoundary(m_text, m_caret);
        moved = true;
    }
    if (text.home && m_caret != 0) {
        m_caret = 0;
        moved = true;
    }
    if (text.end && m_caret != m_text.size()) {
        m_caret = m_text.size();
        moved = true;
    }
    if (edited) {
        changed();
    } else if (moved) {
        m_blink = 0.0F;
        invalidateVisual();
    }
}

void TextField::onFocusedFrame(f32 deltaSeconds) {
    const bool shown = m_blink < kBlinkSeconds;
    m_blink = std::fmod(m_blink + deltaSeconds, 2.0F * kBlinkSeconds);
    if (shown != (m_blink < kBlinkSeconds)) {
        invalidateVisual();
    }
}

} // namespace vxe::ui
