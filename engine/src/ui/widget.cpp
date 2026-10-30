#include "vxe/ui/widget.hpp"

#include <algorithm>
#include <cmath>

namespace vxe::ui {
namespace {

// The main axis size of a rectangle or size, by direction.
constexpr f32 mainOf(Size size, Direction direction) noexcept {
    return direction == Direction::Row ? size.width : size.height;
}
constexpr f32 crossOf(Size size, Direction direction) noexcept {
    return direction == Direction::Row ? size.height : size.width;
}

Size clampSize(Size size, const LayoutStyle& style) noexcept {
    return {std::clamp(size.width, style.minWidth, std::max(style.minWidth, style.maxWidth)),
            std::clamp(size.height, style.minHeight, std::max(style.minHeight, style.maxHeight))};
}

// Positions snap to whole UI pixels; a size is the distance between its
// snapped edges, so neighbors never overlap or leave a one pixel gap.
f32 snap(f32 value) noexcept {
    return std::round(value);
}

} // namespace

Rect DrawContext::toWindow(const UiRect& rect) const noexcept {
    const auto scale = static_cast<f32>(m_scale);
    return {
        .x = rect.x * scale, .y = rect.y * scale, .width = rect.width * scale, .height = rect.height * scale};
}

void DrawContext::fillRect(const UiRect& rect, u32 color) {
    if ((color >> 24U) == 0 || rect.width <= 0.0F || rect.height <= 0.0F) {
        return;
    }
    m_batch.fillRect(toWindow(rect), color);
}

bool DrawContext::nineSlice(const UiRect& rect, std::string_view image, f32 border) {
    return m_batch.nineSlice(toWindow(rect), image, border, static_cast<f32>(m_scale));
}

bool DrawContext::image(const UiRect& rect, std::string_view image, u32 tint) {
    return m_batch.image(toWindow(rect), image, tint);
}

void DrawContext::pushClip(const UiRect& rect) {
    m_batch.pushClip(toWindow(rect));
}

void DrawContext::popClip() {
    m_batch.popClip();
}

void DrawContext::frameRect(const UiRect& rect, u32 color, f32 thickness) {
    const f32 t = std::min({thickness, rect.width / 2.0F, rect.height / 2.0F});
    fillRect({rect.x, rect.y, rect.width, t}, color);
    fillRect({rect.x, rect.y + rect.height - t, rect.width, t}, color);
    fillRect({rect.x, rect.y + t, t, rect.height - 2.0F * t}, color);
    fillRect({rect.x + rect.width - t, rect.y + t, t, rect.height - 2.0F * t}, color);
}

void DrawContext::text(std::string_view text, f32 x, f32 y, u32 color, bool shadow, u32 textScale) {
    const auto scale = static_cast<f32>(m_scale);
    m_batch.text(text, x * scale, y * scale,
                 {.scale = m_scale * textScale, .color = color, .shadow = shadow});
}

Widget& Widget::addChild(std::unique_ptr<Widget> child) {
    child->m_parent = this;
    m_children.push_back(std::move(child));
    invalidateLayout();
    return *m_children.back();
}

void Widget::clearChildren() {
    if (m_children.empty()) {
        return;
    }
    m_children.clear();
    root().m_structureChanged = true;
    invalidateLayout();
}

void Widget::removeChild(const Widget& child) {
    const auto found =
        std::ranges::find_if(m_children, [&child](const auto& entry) { return entry.get() == &child; });
    if (found == m_children.end()) {
        return;
    }
    m_children.erase(found);
    root().m_structureChanged = true;
    invalidateLayout();
}

void Widget::setInteractive(bool interactive) {
    if (m_interactive != interactive) {
        m_interactive = interactive;
        root().m_structureChanged = true; // Its widgets can no longer be hovered or focused.
        invalidateVisual();
    }
}

void Widget::setVisible(bool visible) {
    if (m_visible != visible) {
        m_visible = visible;
        root().m_structureChanged = true; // A hidden widget can no longer be hovered.
        invalidateLayout();
    }
}

Widget& Widget::root() noexcept {
    Widget* node = this;
    while (node->m_parent != nullptr) {
        node = node->m_parent;
    }
    return *node;
}

void Widget::invalidateLayout() noexcept {
    Widget& top = root();
    top.m_layoutRequested = true;
    top.m_redrawRequested = true;
}

void Widget::invalidateVisual() noexcept {
    root().m_redrawRequested = true;
}

Size Widget::measureContent(const UiEnvironment& /*environment*/, f32 /*availableWidth*/) const {
    return {};
}

Edges Widget::themePadding(const Theme& /*theme*/) const noexcept {
    return {};
}

f32 Widget::themeGap(const Theme& /*theme*/) const noexcept {
    return 0.0F;
}

void Widget::draw(DrawContext& /*context*/) const {}

Edges Widget::padding(const Theme& theme) const noexcept {
    return m_style.padding ? *m_style.padding : themePadding(theme);
}

f32 Widget::gap(const Theme& theme) const noexcept {
    return m_style.gap ? *m_style.gap : themeGap(theme);
}

// The size the widget wants: its own content or its children's, plus
// padding, then the fixed size and limits of its style.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
Size Widget::measure(const UiEnvironment& environment, f32 availableWidth) const {
    if (!m_visible) {
        return {};
    }
    const Edges pad = padding(environment.theme);
    const f32 outerWidth =
        m_style.width != kAuto ? m_style.width : std::min(availableWidth, m_style.maxWidth);
    const f32 innerWidth = std::max(0.0F, outerWidth - pad.left - pad.right);
    Size content = measureContent(environment, innerWidth);
    const Size children = measureChildren(environment, innerWidth);
    content = {std::max(content.width, children.width), std::max(content.height, children.height)};
    Size size{content.width + pad.left + pad.right, content.height + pad.top + pad.bottom};
    if (m_style.width != kAuto) {
        size.width = m_style.width;
    }
    if (m_style.height != kAuto) {
        size.height = m_style.height;
    }
    return clampSize(size, m_style);
}

// The children's size in a row, a column or a stack.
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
Size Widget::measureChildren(const UiEnvironment& environment, f32 innerWidth) const {
    Size content{};
    const Direction direction = m_style.direction;
    const f32 spacing = gap(environment.theme);
    usize shown = 0;
    f32 main = 0.0F;
    f32 cross = 0.0F;
    if (direction == Direction::Stack) {
        // Overlapping children: the largest of them.
        for (const auto& child : m_children) {
            const Size size = child->measure(environment, innerWidth);
            content = {std::max(content.width, size.width), std::max(content.height, size.height)};
        }
    }
    for (const auto& child : m_children) {
        if (!child->m_visible || direction == Direction::Stack) {
            continue;
        }
        // In a column every child may use the full inner width; in a row
        // widths add up, so children measure without a limit.
        const Size size =
            child->measure(environment, direction == Direction::Column ? innerWidth : kUnbounded);
        main += mainOf(size, direction);
        cross = std::max(cross, crossOf(size, direction));
        ++shown;
    }
    if (shown > 0) {
        main += spacing * static_cast<f32>(shown - 1);
        const Size children = direction == Direction::Row ? Size{main, cross} : Size{cross, main};
        content = {std::max(content.width, children.width), std::max(content.height, children.height)};
    }
    return content;
}

namespace {

// One child during arrange: its measured main size, then the final one.
struct Line {
    Widget* widget = nullptr;
    f32 main = 0.0F;
    f32 cross = 0.0F;
};

// Hands out free space (positive: grow, negative: shrink) along the main axis.
void distribute(std::vector<Line>& lines, f32 free, Direction direction) {
    if (free > 0.0F) {
        f32 total = 0.0F;
        for (const Line& line : lines) {
            total += std::as_const(*line.widget).style().grow;
        }
        if (total <= 0.0F) {
            return;
        }
        for (Line& line : lines) {
            const LayoutStyle& style = std::as_const(*line.widget).style();
            const f32 limit = direction == Direction::Row ? style.maxWidth : style.maxHeight;
            line.main = std::min(limit, line.main + free * style.grow / total);
        }
    } else if (free < 0.0F) {
        // Shrinking in proportion to size keeps small widgets (an icon next
        // to a long label) from vanishing first.
        f32 total = 0.0F;
        for (const Line& line : lines) {
            total += std::as_const(*line.widget).style().shrink * line.main;
        }
        if (total <= 0.0F) {
            return;
        }
        for (Line& line : lines) {
            const LayoutStyle& style = std::as_const(*line.widget).style();
            const f32 minimum = direction == Direction::Row ? style.minWidth : style.minHeight;
            line.main = std::max(minimum, line.main + free * style.shrink * line.main / total);
        }
    }
}

} // namespace

// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
void Widget::arrange(const UiEnvironment& environment, const UiRect& rect) {
    m_rect = rect;
    if (m_children.empty()) {
        return;
    }
    const Edges pad = padding(environment.theme);
    const UiRect inner{rect.x + pad.left, rect.y + pad.top, std::max(0.0F, rect.width - pad.left - pad.right),
                       std::max(0.0F, rect.height - pad.top - pad.bottom)};
    arrangeChildren(environment, inner);
}

// The flex layout of the children inside the padding (LayoutStyle).
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
void Widget::arrangeChildren(const UiEnvironment& environment, const UiRect& inner) {
    const Direction direction = m_style.direction;
    if (direction == Direction::Stack) {
        for (const auto& child : m_children) {
            child->arrange(environment, child->m_visible ? inner : UiRect{});
        }
        return;
    }
    const bool row = direction == Direction::Row;
    const f32 innerMain = row ? inner.width : inner.height;
    const f32 innerCross = row ? inner.height : inner.width;

    std::vector<Line> lines;
    lines.reserve(m_children.size());
    for (const auto& child : m_children) {
        if (!child->m_visible) {
            child->m_rect = {};
            continue;
        }
        const Size size = child->measure(environment, row ? kUnbounded : inner.width);
        lines.push_back(
            {.widget = child.get(), .main = mainOf(size, direction), .cross = crossOf(size, direction)});
    }
    if (lines.empty()) {
        return;
    }
    const f32 spacing = gap(environment.theme);
    f32 used = spacing * static_cast<f32>(lines.size() - 1);
    for (const Line& line : lines) {
        used += line.main;
    }
    distribute(lines, innerMain - used, direction);
    used = spacing * static_cast<f32>(lines.size() - 1);
    for (const Line& line : lines) {
        used += line.main;
    }
    const f32 free = std::max(0.0F, innerMain - used);
    f32 position = row ? inner.x : inner.y;
    f32 between = spacing;
    switch (m_style.justify) {
    case Justify::Start:
        break;
    case Justify::Center:
        position += free / 2.0F;
        break;
    case Justify::End:
        position += free;
        break;
    case Justify::SpaceBetween:
        between += lines.size() > 1 ? free / static_cast<f32>(lines.size() - 1) : 0.0F;
        break;
    }
    for (const Line& line : lines) {
        const LayoutStyle& style = std::as_const(*line.widget).style();
        const Align align = style.alignSelf.value_or(m_style.align);
        const f32 fixedCross = row ? style.height : style.width;
        f32 cross = std::min(line.cross, innerCross);
        if (align == Align::Stretch && fixedCross == kAuto) {
            const f32 maximum = row ? style.maxHeight : style.maxWidth;
            const f32 minimum = row ? style.minHeight : style.minWidth;
            cross = std::clamp(innerCross, minimum, std::max(minimum, maximum));
        }
        f32 crossStart = row ? inner.y : inner.x;
        if (align == Align::Center) {
            crossStart += (innerCross - cross) / 2.0F;
        } else if (align == Align::End) {
            crossStart += innerCross - cross;
        }
        const f32 mainStart = snap(position);
        const f32 mainEnd = snap(position + line.main);
        const f32 crossBegin = snap(crossStart);
        const f32 crossEnd = snap(crossStart + cross);
        const UiRect childRect =
            row ? UiRect{mainStart, crossBegin, mainEnd - mainStart, crossEnd - crossBegin}
                : UiRect{crossBegin, mainStart, crossEnd - crossBegin, mainEnd - mainStart};
        line.widget->arrange(environment, childRect);
        position += line.main + between;
    }
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
void Widget::drawTree(DrawContext& context) const {
    if (!m_visible) {
        return;
    }
    draw(context);
    drawChildren(context);
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
void Widget::drawChildren(DrawContext& context) const {
    for (const auto& child : m_children) {
        child->drawTree(context);
    }
}

void Widget::revealInParents() {
    const UiRect target = m_rect;
    for (Widget* node = m_parent; node != nullptr; node = node->m_parent) {
        node->reveal(target);
    }
}

// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
usize Widget::countVisible() const noexcept {
    if (!m_visible) {
        return 0;
    }
    usize count = 1;
    for (const auto& child : m_children) {
        count += child->countVisible();
    }
    return count;
}

} // namespace vxe::ui
