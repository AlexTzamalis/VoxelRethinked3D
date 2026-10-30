#include "vxe/ui/keybinds_view.hpp"

#include <vxe/input/keybinds.hpp>
#include <vxe/ui/dialog.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <utility>

namespace vxe::ui {
namespace {

using input::ActionId;
using input::Binding;
using CaptureOutcome = input::InputSystem::CaptureOutcome;

// Categories in the order Section 17.3 lists them; others follow by name.
constexpr std::array<std::string_view, 5> kCategoryOrder = {"Movement", "Interaction", "Inventory",
                                                            "Interface", "Debug"};

// Wide enough for the four buttons of the conflict question.
constexpr f32 kConflictDialogWidth = 320.0F;

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Actions by category, ordered for the list.
std::vector<std::pair<std::string, std::vector<ActionId>>>
actionsByCategory(const input::InputSystem& input) {
    std::map<std::string, std::vector<ActionId>> categories;
    for (u32 index = 0; index < input.actionCount(); ++index) {
        const ActionId action{.index = index};
        categories[input.action(action).category].push_back(action);
    }
    std::vector<std::pair<std::string, std::vector<ActionId>>> ordered(categories.begin(), categories.end());
    const auto rank = [](const std::string& category) {
        return static_cast<usize>(std::ranges::find(kCategoryOrder, category) - kCategoryOrder.begin());
    };
    std::ranges::stable_sort(ordered,
                             [&rank](const auto& a, const auto& b) { return rank(a.first) < rank(b.first); });
    return ordered;
}

} // namespace

KeybindField::KeybindField(input::InputSystem& input, CaptureHandler onCaptured)
    : m_input(input)
    , m_onCaptured(std::move(onCaptured)) {}

// A capture that outlives the field would call back into nothing.
// NOLINTNEXTLINE(bugprone-exception-escape): the callback below returns at once.
KeybindField::~KeybindField() {
    if (m_capturing) {
        m_capturing = false;
        m_input.cancelCapture();
    }
}

void KeybindField::setLabel(std::string label) {
    if (label != m_label) {
        m_label = std::move(label);
        invalidateVisual();
    }
}

void KeybindField::startCapture() {
    if (m_input.isCapturing()) {
        return;
    }
    m_capturing = true;
    invalidateVisual();
    m_input.beginCapture([this](CaptureOutcome outcome, const Binding& binding) {
        if (!m_capturing) {
            return; // The field is being destroyed.
        }
        m_capturing = false;
        invalidateVisual();
        if (m_onCaptured) {
            const CaptureHandler handler = m_onCaptured;
            handler(outcome, binding);
        }
    });
}

Size KeybindField::measureContent(const UiEnvironment& environment, f32 /*availableWidth*/) const {
    return {kWidth, environment.theme.metrics.buttonHeight};
}

void KeybindField::draw(DrawContext& context) const {
    const Theme& theme = context.theme();
    const UiRect& box = rect();
    const bool highlighted = m_hovered || m_focused || m_capturing;
    context.fillRect(box, highlighted ? theme.colors.focus : theme.colors.fieldBorder);
    context.fillRect({box.x + 1.0F, box.y + 1.0F, box.width - 2.0F, box.height - 2.0F}, theme.colors.field);
    const PixelFont* font = context.font();
    if (font == nullptr) {
        return;
    }
    std::string text = m_label;
    u32 color = theme.colors.text;
    if (m_capturing) {
        text = "Press a key";
        color = theme.colors.focus;
    } else if (text.empty()) {
        text = "None";
        color = theme.colors.textMuted;
    }
    text = font->ellipsize(text, box.width - 6.0F);
    const f32 width = std::ceil(font->measure(text).width);
    context.text(text, box.x + std::floor((box.width - width) / 2.0F),
                 box.y + std::floor((box.height - kFontCellPixels) / 2.0F), color, false);
}

void KeybindField::onHover(bool hovered) {
    m_hovered = hovered;
    invalidateVisual();
}

void KeybindField::onFocus(bool focused) {
    m_focused = focused;
    invalidateVisual();
}

void KeybindField::onRelease(bool inside) {
    if (inside) {
        startCapture();
    }
}

void KeybindField::activate() {
    startCapture();
}

KeybindsView::KeybindsView(input::InputSystem& input, UiContext& context)
    : m_input(input)
    , m_context(context)
    , m_scroll(&buildFrame()) {
    buildPage();
}

// The search row and the scroll view for the list.
ScrollView& KeybindsView::buildFrame() {
    style().gap = 4.0F;
    style().grow = 1.0F;
    auto& top = add<Widget>();
    top.style().direction = Direction::Row;
    top.style().gap = 4.0F;
    auto& search = top.add<TextField>("", "Search actions or keys");
    search.style().grow = 1.0F;
    search.setOnChange([this](const std::string& text) { setFilter(text); });
    top.add<Button>("Reset all", [this] {
        showDialog(m_context, {.title = "Reset all keys?",
                               .message = "Every action goes back to its default keys.",
                               .buttons = {{.text = "Reset all",
                                            .onClick =
                                                [this] {
                                                    m_input.resetAllBindings();
                                                    refresh();
                                                }},
                                           {.text = "Cancel", .cancel = true}}});
    });
    auto& scroll = add<ScrollView>();
    scroll.style().grow = 1.0F;
    scroll.add<Widget>().style().gap = 3.0F;
    return scroll;
}

void KeybindsView::setFilter(std::string_view filter) {
    m_filter = lowered(filter);
    m_scroll->scrollTo(0.0F);
    buildPage();
}

bool KeybindsView::matchesFilter(const input::ActionInfo& info) const {
    if (m_filter.empty() || lowered(info.displayName).find(m_filter) != std::string::npos ||
        lowered(info.category).find(m_filter) != std::string::npos) {
        return true;
    }
    return std::ranges::any_of(info.bindings, [this](const Binding& binding) {
        return lowered(input::bindingLabel(binding)).find(m_filter) != std::string::npos;
    });
}

void KeybindsView::buildPage() {
    Widget& page = *m_scroll->children().front();
    page.clearChildren();
    m_rows.clear();
    for (const auto& [category, actions] : actionsByCategory(m_input)) {
        std::vector<ActionId> shown;
        std::ranges::copy_if(actions, std::back_inserter(shown),
                             [this](ActionId action) { return matchesFilter(m_input.action(action)); });
        if (shown.empty()) {
            continue;
        }
        auto& header = page.add<Widget>();
        header.style().direction = Direction::Row;
        header.style().align = Align::Center;
        header.style().padding = Edges{0.0F, 4.0F, 0.0F, 1.0F};
        header.add<Label>(category, TextRole::Title).style().grow = 1.0F;
        auto& reset = header.add<Button>("Reset group", [this, name = category] {
            static_cast<void>(m_input.resetCategory(name));
            refresh();
        });
        reset.style().minWidth = 0.0F;
        for (const ActionId action : shown) {
            addRow(page, action);
        }
    }
    if (m_rows.empty()) {
        page.add<Label>("No action matches the search.", TextRole::Muted);
    }
    refresh();
}

void KeybindsView::addRow(Widget& page, ActionId action) {
    auto& row = page.add<Widget>();
    row.style().direction = Direction::Row;
    row.style().align = Align::Center;
    row.style().gap = 4.0F;
    auto& name = row.add<Label>(m_input.action(action).displayName);
    name.style().grow = 1.0F;
    name.style().minWidth = 0.0F;
    name.setTooltip(name.text());
    auto& warning = row.add<Label>();
    warning.style().width = 5.0F;
    warning.style().shrink = 0.0F;
    Row entry{.action = action, .warning = &warning};
    for (usize slot = 0; slot < kSlots; ++slot) {
        auto& field = row.add<KeybindField>(
            m_input, [this, action, slot](CaptureOutcome outcome, const Binding& binding) {
                captured(action, slot, outcome, binding);
            });
        field.style().shrink = 0.0F;
        field.setTooltip("Click, then press a key. Escape cancels, Backspace clears.");
        entry.fields.at(slot) = &field;
    }
    entry.reset = &row.add<Button>("Reset", [this, action] {
        m_input.resetBindings(action);
        refresh();
    });
    entry.reset->style().minWidth = 0.0F;
    entry.reset->style().shrink = 0.0F;
    m_rows.push_back(entry);
}

void KeybindsView::refresh() {
    for (const Row& row : m_rows) {
        const input::ActionInfo& info = m_input.action(row.action);
        for (usize slot = 0; slot < kSlots; ++slot) {
            row.fields.at(slot)->setLabel(input::bindingLabel(info.bindings.at(slot)));
        }
        row.reset->setEnabled(info.bindings != info.defaults);
        // Which other actions a binding of this row clashes with.
        std::string others;
        for (const Binding& binding : info.bindings) {
            if (binding.isNone()) {
                continue;
            }
            for (const auto& conflict : m_input.findConflicts(binding, info.context, row.action)) {
                others += (others.empty() ? "" : ", ") + m_input.action(conflict.action).displayName;
            }
        }
        row.warning->setText(others.empty() ? "" : "!");
        row.warning->setColor(m_context.theme().colors.focus);
        row.warning->setTooltip(others.empty() ? "" : "Also used by: " + others);
    }
}

void KeybindsView::captured(ActionId action, usize slot, CaptureOutcome outcome, const Binding& binding) {
    if (outcome == CaptureOutcome::Cancelled) {
        return;
    }
    if (outcome == CaptureOutcome::Cleared) {
        m_input.clearBinding(action, slot);
        refresh();
        return;
    }
    const input::ActionInfo& info = m_input.action(action);
    if (info.bindings.at(slot) == binding) {
        return;
    }
    auto conflicts = m_input.findConflicts(binding, info.context, action);
    if (conflicts.empty()) {
        m_input.setBinding(action, slot, binding);
        refresh();
        return;
    }
    m_pending = {.action = action, .slot = slot, .binding = binding, .conflicts = std::move(conflicts)};
    askAboutConflict();
}

void KeybindsView::askAboutConflict() {
    std::string users;
    for (const auto& conflict : m_pending.conflicts) {
        users += (users.empty() ? "" : ", ") + m_input.action(conflict.action).displayName;
    }
    std::vector<DialogButton> buttons;
    // Swapping only makes sense with one other action to trade with.
    if (m_pending.conflicts.size() == 1) {
        buttons.push_back({.text = "Swap", .onClick = [this] {
                               const auto& other = m_pending.conflicts.front();
                               m_input.swapBindings(m_pending.action, m_pending.slot, other.action,
                                                    other.slot);
                               refresh();
                           }});
    }
    buttons.push_back({.text = "Unbind other", .onClick = [this] {
                           for (const auto& conflict : m_pending.conflicts) {
                               m_input.clearBinding(conflict.action, conflict.slot);
                           }
                           m_input.setBinding(m_pending.action, m_pending.slot, m_pending.binding);
                           refresh();
                       }});
    buttons.push_back({.text = "Keep both", .onClick = [this] {
                           m_input.setBinding(m_pending.action, m_pending.slot, m_pending.binding);
                           refresh();
                       }});
    buttons.push_back({.text = "Cancel", .cancel = true});
    showDialog(m_context,
               {.title = "Key already used",
                .message = std::format("{} is used by {}.", input::bindingLabel(m_pending.binding), users),
                .buttons = std::move(buttons),
                .width = kConflictDialogWidth});
}

} // namespace vxe::ui
