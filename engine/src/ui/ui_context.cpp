#include "vxe/ui/ui_context.hpp"

#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

namespace vxe::ui {
namespace {

// Space between a tooltip's frame and its text, and between the pointer and
// the tooltip, in UI pixels.
constexpr f32 kTooltipPadding = 4.0F;
constexpr f32 kTooltipOffsetX = 8.0F;
constexpr f32 kTooltipOffsetY = 12.0F;

// The topmost widget under the point that matches `accept`: children are
// searched last to first, because later children are drawn on top.
// Recursion depth is the depth of the widget tree, a handful of levels.
template <typename Accept>
// NOLINTNEXTLINE(misc-no-recursion)
Widget* findAt(Widget& widget, f32 x, f32 y, Accept accept) {
    if (!widget.visible() || !widget.interactive() || !widget.rect().contains(x, y)) {
        return nullptr;
    }
    for (const auto& child : std::views::reverse(widget.children())) {
        if (Widget* found = findAt(*child, x, y, accept)) {
            return found;
        }
    }
    return accept(widget) ? &widget : nullptr;
}

// Every visible focusable widget, in tree order (the Tab order).
// NOLINTNEXTLINE(misc-no-recursion): bounded by the depth of the widget tree.
void collectFocusable(Widget& widget, std::vector<Widget*>& out) {
    if (!widget.visible() || !widget.interactive()) {
        return;
    }
    if (widget.focusable()) {
        out.push_back(&widget);
    }
    for (const auto& child : widget.children()) {
        collectFocusable(*child, out);
    }
}

// The focusable widget nearest to `from` in a direction (dx, dy one of the
// unit axes). Distance along the direction counts once, distance across it
// twice, so the arrows prefer what lines up with the current widget.
Widget* nearestInDirection(const std::vector<Widget*>& candidates, const Widget& from, f32 dx, f32 dy) {
    const UiRect& origin = from.rect();
    const f32 fx = origin.x + origin.width / 2.0F;
    const f32 fy = origin.y + origin.height / 2.0F;
    Widget* best = nullptr;
    f32 bestScore = std::numeric_limits<f32>::max();
    for (Widget* candidate : candidates) {
        if (candidate == &from) {
            continue;
        }
        const UiRect& box = candidate->rect();
        const f32 cx = box.x + box.width / 2.0F - fx;
        const f32 cy = box.y + box.height / 2.0F - fy;
        const f32 along = cx * dx + cy * dy;
        if (along <= 0.5F) {
            continue;
        }
        const f32 across = std::abs(cx * dy - cy * dx);
        const f32 score = along + 2.0F * across;
        if (score < bestScore) {
            best = candidate;
            bestScore = score;
        }
    }
    return best;
}

} // namespace

UiContext::UiContext(const UiContextDesc& desc)
    : m_stats(desc.stats)
    , m_scaleSetting(desc.settings->registerInt(
          {{.id = "Interface.UiScale",
            .displayName = "Interface scale",
            .description = "Size of menus and the HUD in whole steps, so pixel art stays sharp. 0 picks the "
                           "largest that fits the window."},
           0,
           0,
           kMaxScale,
           1})) {
    if (m_stats == nullptr) {
        return;
    }
    const auto stat = [this](std::string_view name, std::string_view description, stats::StatType type,
                             stats::StatUnit unit = stats::StatUnit::Count) {
        auto id =
            m_stats->registerStat({.name = name, .description = description, .type = type, .unit = unit});
        return id ? id.value() : stats::StatId{};
    };
    m_statIds = {.widgets = stat("Ui.Widgets", "Visible widgets of the game UI.", stats::StatType::Gauge),
                 .scale = stat("Ui.Scale", "Window pixels per UI pixel.", stats::StatType::Gauge),
                 .layoutMs = stat("Ui.LayoutMs", "Time of the last UI layout.", stats::StatType::Gauge,
                                  stats::StatUnit::Milliseconds),
                 .layouts = stat("Ui.Layouts", "UI layouts (only when the tree, window or scale changed).",
                                 stats::StatType::Counter),
                 .rebuilds =
                     stat("Ui.Rebuilds", "Rebuilds of the kept UI quads (only when something changed).",
                          stats::StatType::Counter)};
}

std::unique_ptr<UiContext> UiContext::create(const UiContextDesc& desc) {
    return std::unique_ptr<UiContext>(new UiContext(desc));
}

void UiContext::setTheme(Theme theme) {
    m_theme = theme;
    m_cacheValid = false;
    m_root.invalidateLayout(); // Sizes and paddings come from the theme.
    for (const Layer& layer : m_layers) {
        layer.root->invalidateLayout();
    }
}

u32 UiContext::autoScale(u32 windowWidth, u32 windowHeight) noexcept {
    const u32 fit = std::min(windowWidth / kReferenceWidth, windowHeight / kReferenceHeight);
    return std::clamp<u32>(fit, 1U, static_cast<u32>(kMaxScale));
}

Widget& UiContext::pushLayer(std::unique_ptr<Widget> layer, LayerOptions options) {
    Widget& result = *layer;
    m_layers.push_back({.root = std::move(layer),
                        .options = std::move(options),
                        .focusBefore = m_focused,
                        .generationBefore = m_generation});
    // The new layer starts without focus; the dialog code may set one.
    setFocus(nullptr);
    m_hovered = nullptr;
    m_pressed = nullptr;
    m_cacheValid = false;
    result.invalidateLayout();
    return result;
}

void UiContext::closeLayer(const Widget& layer) {
    for (Layer& entry : m_layers) {
        if (entry.root.get() == &layer) {
            entry.closing = true;
        }
    }
}

void UiContext::closePendingLayers() {
    for (usize index = m_layers.size(); index-- > 0;) {
        if (!m_layers[index].closing) {
            continue;
        }
        const Layer closed = std::move(m_layers[index]);
        m_layers.erase(m_layers.begin() + static_cast<std::ptrdiff_t>(index));
        forgetWidgets();
        // Back to the widget focused before the layer opened, if nothing was
        // removed since (it might be gone otherwise).
        if (closed.generationBefore == m_generation && closed.focusBefore != nullptr) {
            setFocus(closed.focusBefore);
        }
        m_cacheValid = false;
    }
}

void UiContext::forgetWidgets() noexcept {
    m_hovered = nullptr;
    m_pressed = nullptr;
    m_focused = nullptr;
    m_tipWidget = nullptr;
    m_tipTime = 0.0F;
}

void UiContext::setFocus(Widget* widget) {
    if (widget == m_focused) {
        return;
    }
    if (m_focused != nullptr) {
        m_focused->onFocus(false);
    }
    m_focused = widget;
    m_focusFromPointer = false;
    if (m_focused != nullptr) {
        m_focused->onFocus(true);
    }
}

bool UiContext::wantsTextInput() const noexcept {
    return m_focused != nullptr && m_focused->capturesText();
}

Widget& UiContext::activeRoot() noexcept {
    for (const Layer& layer : std::views::reverse(m_layers)) {
        if (layer.options.modal) {
            return *layer.root;
        }
    }
    return m_root;
}

void UiContext::layout(u32 windowWidth, u32 windowHeight, const UiRenderer& renderer) {
    VXE_PROFILE_FUNCTION();
    const Stopwatch stopwatch;
    const UiEnvironment environment{.theme = m_theme, .font = renderer.font()};
    // Every root covers the window in UI pixels; a window that is not a
    // multiple of the scale leaves a sliver at the right and bottom.
    const auto scale = static_cast<f32>(m_scale);
    const f32 width = std::floor(static_cast<f32>(windowWidth) / scale);
    const f32 height = std::floor(static_cast<f32>(windowHeight) / scale);
    m_root.arrange(environment, {0.0F, 0.0F, width, height});
    for (const Layer& layer : m_layers) {
        layer.root->arrange(environment, {0.0F, 0.0F, width, height});
    }
    m_cacheValid = false;
    if (m_stats != nullptr) {
        if (m_statIds.layoutMs.isValid()) {
            m_stats->set(m_statIds.layoutMs, stopwatch.elapsedMilliseconds());
        }
        if (m_statIds.layouts.isValid()) {
            m_stats->add(m_statIds.layouts);
        }
    }
}

void UiContext::update(u32 windowWidth, u32 windowHeight, const UiInput& input, const UiRenderer& renderer) {
    closePendingLayers();
    // Clicks and keys of the last frame may have removed widgets; then no
    // remembered widget may be touched again.
    checkStructure();
    const i32 chosen = m_scaleSetting.get();
    const u32 scale = chosen > 0 ? static_cast<u32>(chosen) : autoScale(windowWidth, windowHeight);
    // Every flag is taken every frame so none stays set.
    bool requested = m_root.takeLayoutRequest();
    for (const Layer& layer : m_layers) {
        requested = layer.root->takeLayoutRequest() || requested;
    }
    if (requested || scale != m_scale || windowWidth != m_width || windowHeight != m_height ||
        renderer.resourceVersion() != m_fontVersion) {
        m_scale = scale;
        m_width = windowWidth;
        m_height = windowHeight;
        m_fontVersion = renderer.resourceVersion();
        layout(windowWidth, windowHeight, renderer);
    }
    if (m_focused != nullptr) {
        m_focused->onFocusedFrame(input.deltaSeconds);
    }
    updateTooltip(input);
    routePointer(input.pointer);
    checkStructure();
    navigate(input);
    if (m_stats != nullptr) {
        if (m_statIds.widgets.isValid()) {
            m_stats->set(m_statIds.widgets, static_cast<f64>(std::max<usize>(m_root.countVisible(), 1) - 1));
        }
        if (m_statIds.scale.isValid()) {
            m_stats->set(m_statIds.scale, static_cast<f64>(m_scale));
        }
    }
}

void UiContext::routePointer(const PointerInput& pointer) {
    Widget* target = nullptr;
    const auto scale = static_cast<f32>(m_scale);
    const f32 x = pointer.x / scale;
    const f32 y = pointer.y / scale;
    const bool moved = x != m_pointerX || y != m_pointerY;
    m_pointerX = x;
    m_pointerY = y;
    Widget& root = activeRoot();
    const bool modalOpen = &root != &m_root;
    m_overBlocking = pointer.available && modalOpen;
    if (pointer.available) {
        target = findAt(root, x, y, [](const Widget& widget) { return widget.takesPointer(); });
        m_overBlocking = m_overBlocking || findAt(root, x, y, [](const Widget& widget) {
                                               return widget.blocksPointer();
                                           }) != nullptr;
    }
    if (target != m_hovered) {
        if (m_hovered != nullptr) {
            m_hovered->onHover(false);
        }
        m_hovered = target;
        if (m_hovered != nullptr) {
            m_hovered->onHover(true);
        }
    }
    // Hover moves the focus, so mouse and keyboard agree on what Enter
    // presses; a text field keeps it until something else is clicked.
    if (pointer.available && moved && (m_focused == nullptr || !m_focused->capturesText())) {
        if (target != nullptr && target->focusable()) {
            setFocus(target);
            m_focusFromPointer = true;
        } else if (m_focusFromPointer) {
            setFocus(nullptr);
        }
    }
    const bool down = pointer.available && pointer.primaryDown;
    if (down && !m_wasDown) {
        setFocus(target != nullptr && target->focusable() ? target : nullptr);
        m_focusFromPointer = target != nullptr && !target->capturesText();
        if (m_hovered != nullptr) {
            m_pressed = m_hovered;
            m_pressed->onPress(x, y);
        }
    } else if (!down && m_wasDown && m_pressed != nullptr) {
        // Forget the widget before telling it: its click may destroy it.
        Widget* released = std::exchange(m_pressed, nullptr);
        released->onRelease(released == m_hovered);
    }
    m_wasDown = down;
}

void UiContext::checkStructure() {
    bool changed = m_root.takeStructureChange();
    for (const Layer& layer : m_layers) {
        changed = layer.root->takeStructureChange() || changed;
    }
    if (changed) {
        forgetWidgets();
        ++m_generation;
    }
}

void UiContext::handleBack() {
    if (m_layers.empty()) {
        m_backRequested = true;
        return;
    }
    Layer& top = m_layers.back();
    if (top.options.onBack) {
        const auto onBack = top.options.onBack;
        onBack();
    } else {
        top.closing = true;
    }
}

// Typing and editing keys go to a focused text field. Returns whether a text
// field (still) has the focus afterwards.
bool UiContext::editText(const TextEditInput& text) {
    if (m_focused == nullptr || !m_focused->capturesText()) {
        return false;
    }
    if (!text.typed.empty() || text.erase || text.eraseNext || text.caretLeft || text.caretRight ||
        text.home || text.end) {
        m_focused->onTextInput(text);
        checkStructure(); // The field's change handler may have rebuilt the screen.
    }
    return m_focused != nullptr && m_focused->capturesText();
}

// Tab, Shift+Tab and the arrows. Returns whether one of them was pressed.
bool UiContext::moveFocus(const NavigationInput& keys, const std::vector<Widget*>& focusable,
                          bool textFocus) {
    const auto current = std::ranges::find(focusable, m_focused);
    const bool hasFocus = current != focusable.end();
    if (keys.next || keys.previous) {
        usize index = keys.previous ? focusable.size() - 1 : 0;
        if (hasFocus) {
            const auto at = static_cast<usize>(current - focusable.begin());
            index = (at + (keys.next ? 1 : focusable.size() - 1)) % focusable.size();
        }
        setFocus(focusable[index]);
        return true;
    }
    // Left and Right move a text field's caret instead.
    f32 dx = 0.0F;
    f32 dy = 0.0F;
    if (keys.up || keys.down) {
        dy = keys.down ? 1.0F : -1.0F;
    } else if (!textFocus && (keys.left || keys.right)) {
        dx = keys.right ? 1.0F : -1.0F;
    } else {
        return false;
    }
    if (!hasFocus) {
        setFocus(focusable.front());
    } else if (Widget* next = nearestInDirection(focusable, *m_focused, dx, dy)) {
        setFocus(next);
    }
    return true;
}

void UiContext::navigate(const UiInput& input) {
    const NavigationInput& keys = input.navigation;
    if (keys.back) {
        handleBack();
        return;
    }
    const bool textFocus = editText(input.text);
    std::vector<Widget*> focusable;
    collectFocusable(activeRoot(), focusable);
    if (focusable.empty() || moveFocus(keys, focusable, textFocus)) {
        return;
    }
    if (keys.activate && std::ranges::find(focusable, m_focused) != focusable.end()) {
        // Last: activating may change or remove the tree.
        m_focused->activate();
    }
}

void UiContext::updateTooltip(const UiInput& input) {
    const Widget* candidate = nullptr;
    std::string_view text;
    f32 anchorX = 0.0F;
    f32 anchorY = 0.0F;
    if (input.pointer.available) {
        const auto scale = static_cast<f32>(m_scale);
        const f32 x = input.pointer.x / scale;
        const f32 y = input.pointer.y / scale;
        // Any widget with a tooltip, even a disabled button, so a disabled
        // control can say why.
        candidate =
            findAt(activeRoot(), x, y, [](const Widget& widget) { return !widget.tooltip().empty(); });
        anchorX = x + kTooltipOffsetX;
        anchorY = y + kTooltipOffsetY;
    }
    if (candidate == nullptr && m_focused != nullptr && !m_focusFromPointer &&
        !m_focused->tooltip().empty()) {
        // Focus moved with the keyboard: the tooltip shows below the widget.
        candidate = m_focused;
        anchorX = m_focused->rect().x;
        anchorY = m_focused->rect().y + m_focused->rect().height + 2.0F;
    }
    if (candidate != nullptr) {
        text = candidate->tooltip();
    }
    if (candidate != m_tipWidget) {
        m_tipWidget = candidate;
        m_tipTime = 0.0F;
        m_tipX = anchorX;
        m_tipY = anchorY;
        m_cacheValid = false; // The tooltip is part of the kept quads.
        m_tipText.assign(text);
        return;
    }
    if (m_tipWidget == nullptr) {
        return;
    }
    const bool wasVisible = m_tipTime >= kTooltipDelaySeconds;
    m_tipTime += input.deltaSeconds;
    if (!wasVisible) {
        // The tooltip appears where the pointer rests, then stays put.
        m_tipX = anchorX;
        m_tipY = anchorY;
        if (m_tipTime >= kTooltipDelaySeconds) {
            m_cacheValid = false;
        }
    }
    if (m_tipText != text) {
        m_tipText.assign(text);
        m_cacheValid = false;
    }
}

std::string_view UiContext::visibleTooltip() const noexcept {
    return m_tipWidget != nullptr && m_tipTime >= kTooltipDelaySeconds ? std::string_view(m_tipText)
                                                                       : std::string_view();
}

void UiContext::drawTooltip(DrawContext& context) const {
    const std::string_view text = visibleTooltip();
    const PixelFont* font = context.font();
    if (text.empty() || font == nullptr) {
        return;
    }
    const std::string lines = font->wrap(text, m_theme.metrics.tooltipMaxWidth);
    const TextSize size = font->measure(lines);
    const f32 width = std::ceil(size.width) + 2.0F * kTooltipPadding;
    const f32 height = std::ceil(size.height) + 2.0F * kTooltipPadding;
    // Kept inside the window: flipped to the other side near the edges.
    const f32 screenWidth = std::floor(static_cast<f32>(m_width) / static_cast<f32>(m_scale));
    const f32 screenHeight = std::floor(static_cast<f32>(m_height) / static_cast<f32>(m_scale));
    f32 x = m_tipX;
    f32 y = m_tipY;
    if (x + width > screenWidth) {
        x = std::max(0.0F, screenWidth - width);
    }
    if (y + height > screenHeight) {
        y = std::max(0.0F, m_tipY - height - 2.0F * kTooltipOffsetY);
    }
    const UiRect box{std::round(x), std::round(y), width, height};
    context.fillRect(box, m_theme.colors.tooltipBorder);
    const f32 border = m_theme.metrics.border;
    context.fillRect({box.x + border, box.y + border, box.width - 2.0F * border, box.height - 2.0F * border},
                     m_theme.colors.tooltip);
    context.text(lines, box.x + kTooltipPadding, box.y + kTooltipPadding, m_theme.colors.text, true);
}

void UiContext::draw(UiRenderer& renderer) {
    VXE_PROFILE_FUNCTION();
    bool redraw = m_root.takeRedrawRequest();
    for (const Layer& layer : m_layers) {
        redraw = layer.root->takeRedrawRequest() || redraw;
    }
    if (redraw || !m_cacheValid || m_cacheVersion != renderer.resourceVersion()) {
        m_cache.reset(&renderer.resources());
        DrawContext context(m_cache, m_theme, m_scale);
        m_root.drawTree(context);
        const UiRect window{0.0F, 0.0F, static_cast<f32>(m_width) / static_cast<f32>(m_scale) + 1.0F,
                            static_cast<f32>(m_height) / static_cast<f32>(m_scale) + 1.0F};
        for (const Layer& layer : m_layers) {
            if (layer.options.modal) {
                context.fillRect(window, m_theme.colors.backdrop);
            }
            layer.root->drawTree(context);
        }
        drawTooltip(context);
        m_cacheVersion = renderer.resourceVersion();
        m_cacheValid = true;
        ++m_rebuilds;
        if (m_stats != nullptr && m_statIds.rebuilds.isValid()) {
            m_stats->add(m_statIds.rebuilds);
        }
    }
    renderer.frame().append(m_cache);
}

} // namespace vxe::ui
