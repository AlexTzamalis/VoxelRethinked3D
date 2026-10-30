#include "vxe/ui/widgets.hpp"

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

// Measuring also fixes the lines the label draws: the layout measures every
// widget at the width it will get, the last measure before drawing wins.
Size Label::measureContent(const UiEnvironment& environment, f32 availableWidth) const {
    if (environment.font == nullptr || m_text.empty()) {
        m_lines.clear();
        m_linesSize = {};
        return {};
    }
    m_lines = m_wrap && availableWidth < kUnbounded ? environment.font->wrap(m_text, availableWidth) : m_text;
    const TextSize size = environment.font->measure(m_lines);
    m_linesSize = {std::ceil(size.width), std::ceil(size.height)};
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
    context.text(m_lines, x, y, color, m_shadow);
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
    } else if (m_hovered) {
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
    } else if (m_hovered) {
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
    if (m_hovered && m_enabled) {
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

} // namespace vxe::ui
