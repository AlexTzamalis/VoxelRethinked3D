#include "vxe/ui/ui_context.hpp"

#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace vxe::ui {
namespace {

// The topmost widget under the point that matches `accept`: children are
// searched last to first, because later children are drawn on top.
// Recursion depth is the depth of the widget tree, a handful of levels.
template <typename Accept>
// NOLINTNEXTLINE(misc-no-recursion)
Widget* findAt(Widget& widget, f32 x, f32 y, Accept accept) {
    if (!widget.visible() || !widget.rect().contains(x, y)) {
        return nullptr;
    }
    for (const auto& child : std::views::reverse(widget.children())) {
        if (Widget* found = findAt(*child, x, y, accept)) {
            return found;
        }
    }
    return accept(widget) ? &widget : nullptr;
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
                                 stats::StatType::Counter)};
}

std::unique_ptr<UiContext> UiContext::create(const UiContextDesc& desc) {
    return std::unique_ptr<UiContext>(new UiContext(desc));
}

void UiContext::setTheme(Theme theme) {
    m_theme = theme;
    m_root.invalidateLayout(); // Sizes and paddings come from the theme.
}

u32 UiContext::autoScale(u32 windowWidth, u32 windowHeight) noexcept {
    const u32 fit = std::min(windowWidth / kReferenceWidth, windowHeight / kReferenceHeight);
    return std::clamp<u32>(fit, 1U, static_cast<u32>(kMaxScale));
}

void UiContext::layout(u32 windowWidth, u32 windowHeight, const UiRenderer& renderer) {
    VXE_PROFILE_FUNCTION();
    const Stopwatch stopwatch;
    const UiEnvironment environment{.theme = m_theme, .font = renderer.font()};
    // The root covers the window in UI pixels; a window that is not a
    // multiple of the scale leaves a sliver at the right and bottom.
    const auto scale = static_cast<f32>(m_scale);
    const f32 width = std::floor(static_cast<f32>(windowWidth) / scale);
    const f32 height = std::floor(static_cast<f32>(windowHeight) / scale);
    m_root.arrange(environment, {0.0F, 0.0F, width, height});
    if (m_stats != nullptr) {
        if (m_statIds.layoutMs.isValid()) {
            m_stats->set(m_statIds.layoutMs, stopwatch.elapsedMilliseconds());
        }
        if (m_statIds.layouts.isValid()) {
            m_stats->add(m_statIds.layouts);
        }
    }
}

void UiContext::update(u32 windowWidth, u32 windowHeight, const PointerInput& pointer,
                       const UiRenderer& renderer) {
    const i32 chosen = m_scaleSetting.get();
    const u32 scale = chosen > 0 ? static_cast<u32>(chosen) : autoScale(windowWidth, windowHeight);
    // Both flags are taken every frame so neither stays set.
    const bool requested = m_root.takeLayoutRequest();
    if (m_root.takeStructureChange()) {
        // Removed or hidden widgets may be the hovered or pressed one.
        m_hovered = nullptr;
        m_pressed = nullptr;
    }
    if (requested || scale != m_scale || windowWidth != m_width || windowHeight != m_height ||
        renderer.fontVersion() != m_fontVersion) {
        m_scale = scale;
        m_width = windowWidth;
        m_height = windowHeight;
        m_fontVersion = renderer.fontVersion();
        layout(windowWidth, windowHeight, renderer);
    }
    routePointer(pointer);
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
    m_overBlocking = false;
    if (pointer.available) {
        const f32 x = pointer.x / static_cast<f32>(m_scale);
        const f32 y = pointer.y / static_cast<f32>(m_scale);
        target = findAt(m_root, x, y, [](const Widget& widget) { return widget.takesPointer(); });
        m_overBlocking =
            findAt(m_root, x, y, [](const Widget& widget) { return widget.blocksPointer(); }) != nullptr;
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
    const bool down = pointer.available && pointer.primaryDown;
    if (down && !m_wasDown && m_hovered != nullptr) {
        m_pressed = m_hovered;
        m_pressed->onPress();
    } else if (!down && m_wasDown && m_pressed != nullptr) {
        // Forget the widget before telling it: its click may destroy it.
        Widget* released = std::exchange(m_pressed, nullptr);
        released->onRelease(released == m_hovered);
    }
    m_wasDown = down;
}

void UiContext::draw(UiRenderer& renderer) const {
    VXE_PROFILE_FUNCTION();
    DrawContext context(renderer, m_theme, m_scale);
    m_root.drawTree(context);
}

} // namespace vxe::ui
