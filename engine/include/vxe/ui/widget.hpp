#pragma once

#include <vxe/core/types.hpp>
#include <vxe/ui/pixel_font.hpp>
#include <vxe/ui/quad_batch.hpp>
#include <vxe/ui/theme.hpp>

#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vxe::ui {

struct TextEditInput;

// Layout runs in UI pixels; the UI scale (a whole number) turns them into
// window pixels when drawing, so every UI pixel covers the same square of
// window pixels and pixel art and text stay crisp (E12.1).
inline constexpr f32 kAuto = -1.0F;
inline constexpr f32 kUnbounded = std::numeric_limits<f32>::infinity();

enum class Direction : u8 {
    Row,
    Column,
    // Children overlap: each gets the whole inner rectangle (screens on top
    // of each other, a label over an image).
    Stack
};
// Where children sit along the main axis when they do not fill it.
enum class Justify : u8 {
    Start,
    Center,
    End,
    SpaceBetween
};
// Where children sit across the main axis; Stretch fills it.
enum class Align : u8 {
    Start,
    Center,
    End,
    Stretch
};

struct Edges {
    constexpr Edges() noexcept = default;
    constexpr Edges(f32 leftEdge, f32 topEdge, f32 rightEdge, f32 bottomEdge) noexcept
        : left(leftEdge)
        , top(topEdge)
        , right(rightEdge)
        , bottom(bottomEdge) {}
    f32 left = 0.0F;
    f32 top = 0.0F;
    f32 right = 0.0F;
    f32 bottom = 0.0F;
    [[nodiscard]] static constexpr Edges all(f32 value) noexcept { return {value, value, value, value}; }
    [[nodiscard]] static constexpr Edges symmetric(f32 horizontal, f32 vertical) noexcept {
        return {horizontal, vertical, horizontal, vertical};
    }
};

struct Size {
    constexpr Size() noexcept = default;
    constexpr Size(f32 sizeWidth, f32 sizeHeight) noexcept
        : width(sizeWidth)
        , height(sizeHeight) {}
    f32 width = 0.0F;
    f32 height = 0.0F;
};

// A rectangle in UI pixels, origin at the top left of the window, y down.
struct UiRect {
    constexpr UiRect() noexcept = default;
    constexpr UiRect(f32 left, f32 top, f32 rectWidth, f32 rectHeight) noexcept
        : x(left)
        , y(top)
        , width(rectWidth)
        , height(rectHeight) {}
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;
    [[nodiscard]] bool contains(f32 px, f32 py) const noexcept {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

// The flexbox subset of Section 19.1. A widget lays out its children in a
// row or a column: each child gets its measured size along the main axis,
// leftover space goes to children with `grow` (in proportion), missing space
// is taken from children with `shrink` (in proportion to their size, never
// below their minimum), and `justify` places children that do not fill the
// line. Across the main axis `align` (or a child's `alignSelf`) places or
// stretches each child. Padding and gap left unset take the widget's theme
// default (a Panel uses the theme's panel padding and gap).
struct LayoutStyle {
    Direction direction = Direction::Column;
    Justify justify = Justify::Start;
    Align align = Align::Stretch;
    std::optional<Align> alignSelf;
    std::optional<Edges> padding;
    std::optional<f32> gap;
    f32 grow = 0.0F;
    f32 shrink = 1.0F;
    f32 width = kAuto; // A fixed size, or kAuto for the measured one.
    f32 height = kAuto;
    f32 minWidth = 0.0F;
    f32 minHeight = 0.0F;
    f32 maxWidth = kUnbounded;
    f32 maxHeight = kUnbounded;
};

// What measuring and drawing need besides the widget itself.
struct UiEnvironment {
    const Theme& theme;
    const PixelFont* font = nullptr; // Nothing before the first font loaded: text measures as empty.
};

// Draws in UI pixels into a QuadBatch; converts to window pixels with the scale.
class DrawContext {
public:
    DrawContext(QuadBatch& batch, const Theme& theme, u32 scale) noexcept
        : m_batch(batch)
        , m_theme(theme)
        , m_scale(scale) {}
    void fillRect(const UiRect& rect, u32 color);
    // A rectangle outline `thickness` UI pixels wide, inside the rectangle.
    void frameRect(const UiRect& rect, u32 color, f32 thickness = 1.0F);
    // Text with its top left corner at (x, y): one font pixel per UI pixel.
    void text(std::string_view text, f32 x, f32 y, u32 color, bool shadow = true);
    // A theme image as a nine slice frame with corners `border` texels (UI
    // pixels) wide. False when no pack has the image: draw the flat look.
    bool nineSlice(const UiRect& rect, std::string_view image, f32 border);
    // Clips everything until popClip() to the rectangle (scroll views).
    void pushClip(const UiRect& rect);
    void popClip();
    [[nodiscard]] const Theme& theme() const noexcept { return m_theme; }
    [[nodiscard]] const PixelFont* font() const noexcept {
        return m_batch.resources() != nullptr && m_batch.resources()->font ? &*m_batch.resources()->font
                                                                           : nullptr;
    }

private:
    [[nodiscard]] Rect toWindow(const UiRect& rect) const noexcept;

    QuadBatch& m_batch;
    const Theme& m_theme;
    u32 m_scale;
};

// A node of the retained UI tree (Section 19.1). A plain Widget is an
// invisible container (a row, a column, a spacer with grow); subclasses
// draw something (Panel, Label, Button in widgets.hpp) and may react to the
// pointer. The tree owns its children; UiContext owns the root.
//
// Changing anything that affects sizes (text, style, children) calls
// invalidateLayout(), which flags the root, so the next UiContext::update()
// lays the tree out again; a change of looks only (hover) calls
// invalidateVisual(), so the next draw rebuilds the kept quads. Nothing is
// laid out or rebuilt per frame otherwise.
//
// Thread safety: main thread only.
class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget(Widget&&) = delete;
    Widget& operator=(Widget&&) = delete;

    // Changing the style through this marks the layout for an update.
    [[nodiscard]] LayoutStyle& style() noexcept {
        invalidateLayout();
        return m_style;
    }
    [[nodiscard]] const LayoutStyle& style() const noexcept { return m_style; }

    // Adds a child at the end (drawn last, on top) and returns it.
    template <typename T, typename... Args>
    T& add(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T& result = *child;
        addChild(std::move(child));
        return result;
    }
    Widget& addChild(std::unique_ptr<Widget> child);
    void clearChildren();
    // Removes and destroys one child; nothing when it is not a child.
    void removeChild(const Widget& child);
    [[nodiscard]] std::span<const std::unique_ptr<Widget>> children() const noexcept { return m_children; }
    [[nodiscard]] Widget* parent() const noexcept { return m_parent; }

    [[nodiscard]] bool visible() const noexcept { return m_visible; }
    // Shown after the pointer rests on the widget (or the keyboard focuses
    // it); details belong here instead of long labels.
    void setTooltip(std::string tooltip) { m_tooltip = std::move(tooltip); }
    [[nodiscard]] const std::string& tooltip() const noexcept { return m_tooltip; }
    void setVisible(bool visible);
    // An inert widget and its children still draw but ignore the pointer,
    // focus and tooltips (a screen below the top one).
    void setInteractive(bool interactive);
    [[nodiscard]] bool interactive() const noexcept { return m_interactive; }
    // Where the last layout put the widget, in UI pixels.
    [[nodiscard]] const UiRect& rect() const noexcept { return m_rect; }

    // Flags the tree for a new layout (and a redraw).
    void invalidateLayout() noexcept;
    // Flags the tree for a redraw only.
    void invalidateVisual() noexcept;
    // On the root: true when the tree must be drawn again; clears the flag.
    [[nodiscard]] bool takeRedrawRequest() noexcept { return std::exchange(m_redrawRequested, false); }
    // On the root: true when the layout must run again; clears the flag.
    [[nodiscard]] bool takeLayoutRequest() noexcept { return std::exchange(m_layoutRequested, false); }
    // On the root: true when widgets were removed since the last call
    // (pointer targets may be gone); clears the flag.
    [[nodiscard]] bool takeStructureChange() noexcept { return std::exchange(m_structureChanged, false); }

    // The size of what the widget itself shows, without padding: text for a
    // label, the label and frame of a button. `availableWidth` is the width
    // the parent can give (kUnbounded in rows), for text that wraps.
    [[nodiscard]] virtual Size measureContent(const UiEnvironment& environment, f32 availableWidth) const;
    // Padding and gap when the style leaves them unset.
    [[nodiscard]] virtual Edges themePadding(const Theme& theme) const noexcept;
    [[nodiscard]] virtual f32 themeGap(const Theme& theme) const noexcept;
    // Draws the widget; the default draws nothing. Children are drawn after.
    virtual void draw(DrawContext& context) const;

    // Pointer handling (UiContext calls these). A widget that takes the
    // pointer is a target for hover and press; one that blocks it hides the
    // world below from the pointer (a panel) without reacting itself.
    [[nodiscard]] virtual bool takesPointer() const noexcept { return false; }
    [[nodiscard]] virtual bool blocksPointer() const noexcept { return takesPointer(); }
    virtual void onHover(bool /*hovered*/) {}
    // The pointer went down over the widget at (x, y) in UI pixels.
    virtual void onPress(f32 /*x*/, f32 /*y*/) {}
    // `inside`: the pointer went up over the widget, which makes it a click.
    virtual void onRelease(bool /*inside*/) {}
    // The pointer moved to (x, y), UI pixels, while pressed on the widget
    // (sliders follow it).
    virtual void onDrag(f32 /*x*/, f32 /*y*/) {}
    // A widget that scrolls takes the mouse wheel over it (and its children).
    [[nodiscard]] virtual bool takesWheel() const noexcept { return false; }
    // Wheel lines, positive away from the user (scroll up).
    virtual void onWheel(f32 /*lines*/) {}

    // Keyboard focus (E12.4). UiContext moves focus with Tab, the arrows,
    // clicks and hovering; the focused widget gets Enter and, if it captures
    // text, the typed text and editing keys.
    [[nodiscard]] virtual bool focusable() const noexcept { return false; }
    // A text field: typing goes to it, and Left and Right move its caret
    // instead of the focus. Hovering other widgets does not take focus away.
    [[nodiscard]] virtual bool capturesText() const noexcept { return false; }
    virtual void onFocus(bool /*focused*/) {}
    // Enter on the focused widget.
    virtual void activate() {}
    virtual void onTextInput(const TextEditInput& /*text*/) {}
    // Every frame while focused, for animations like a blinking caret.
    virtual void onFocusedFrame(f32 /*deltaSeconds*/) {}
    // Left or Right (-1 or 1) on the focused widget. Return true to use them
    // (a slider steps, a choice cycles) instead of moving the focus.
    virtual bool onAdjust(i32 /*direction*/) { return false; }
    // Asks the parents to show `rect` (a widget that got the keyboard focus);
    // scroll views scroll it into view.
    void revealInParents();
    virtual void reveal(const UiRect& /*rect*/) {}

    // The layout pass (widget.cpp); UiContext calls them on the root.
    [[nodiscard]] Size measure(const UiEnvironment& environment, f32 availableWidth) const;
    void arrange(const UiEnvironment& environment, const UiRect& rect);
    void drawTree(DrawContext& context) const;
    // The parts of the pass a container may replace (a scroll view lays its
    // content out taller than itself, clips it and draws a scroll bar).
    [[nodiscard]] virtual Size measureChildren(const UiEnvironment& environment, f32 innerWidth) const;
    virtual void arrangeChildren(const UiEnvironment& environment, const UiRect& inner);
    virtual void drawChildren(DrawContext& context) const;
    [[nodiscard]] usize countVisible() const noexcept;

private:
    [[nodiscard]] Edges padding(const Theme& theme) const noexcept;
    [[nodiscard]] f32 gap(const Theme& theme) const noexcept;
    [[nodiscard]] Widget& root() noexcept;

    LayoutStyle m_style{};
    std::vector<std::unique_ptr<Widget>> m_children;
    Widget* m_parent = nullptr;
    UiRect m_rect{};
    std::string m_tooltip;
    bool m_visible = true;
    bool m_interactive = true;
    bool m_layoutRequested = true;
    bool m_redrawRequested = true;
    bool m_structureChanged = false;
};

} // namespace vxe::ui
