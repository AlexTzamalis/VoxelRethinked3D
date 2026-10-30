#pragma once

#include <vxe/input/input.hpp>
#include <vxe/ui/controls.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/widgets.hpp>

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::ui {

// One binding slot of an action: shows its key, and a click or Enter starts
// listening for a new one (InputSystem::beginCapture, where Escape cancels
// and Backspace clears). Leaving the screen while it listens stops listening.
class KeybindField : public Widget {
public:
    using CaptureHandler = std::function<void(input::InputSystem::CaptureOutcome, const input::Binding&)>;

    KeybindField(input::InputSystem& input, CaptureHandler onCaptured);
    ~KeybindField() override;
    KeybindField(const KeybindField&) = delete;
    KeybindField& operator=(const KeybindField&) = delete;
    KeybindField(KeybindField&&) = delete;
    KeybindField& operator=(KeybindField&&) = delete;

    // The key to show; empty shows that the slot is free.
    void setLabel(std::string label);
    [[nodiscard]] const std::string& label() const noexcept { return m_label; }
    [[nodiscard]] bool capturing() const noexcept { return m_capturing; }

    [[nodiscard]] Size measureContent(const UiEnvironment& environment, f32 availableWidth) const override;
    void draw(DrawContext& context) const override;
    [[nodiscard]] bool takesPointer() const noexcept override { return true; }
    [[nodiscard]] bool focusable() const noexcept override { return true; }
    void onHover(bool hovered) override;
    void onFocus(bool focused) override;
    void onRelease(bool inside) override;
    void activate() override;

    // The width of a binding slot, in UI pixels.
    static constexpr f32 kWidth = 84.0F;

private:
    void startCapture();

    input::InputSystem& m_input;
    CaptureHandler m_onCaptured;
    std::string m_label;
    bool m_capturing = false;
    bool m_hovered = false;
    bool m_focused = false;
};

// The keybind list (Section 17.3): every action by category, with its first
// two binding slots, a warning when a binding clashes with another action,
// and Reset per action, per category and for everything. Picking a key that
// another action uses asks what to do: swap the two, unbind the other, keep
// both or cancel. A search box filters by action, category or key. Changes
// apply at once; the engine writes keybinds.toml when the game closes.
//
// Actions may hold a third binding (Section 17.2). The list does not show it
// because two slots fill the width of the smallest window; it stays in
// keybinds.toml and Reset restores it.
//
// Thread safety: main thread only, like the input system.
class KeybindsView : public Widget {
public:
    KeybindsView(input::InputSystem& input, UiContext& context);

    void setFilter(std::string_view filter);
    // Brings every row up to date with the current bindings.
    void refresh();
    // How many action rows show.
    [[nodiscard]] usize rowCount() const noexcept { return m_rows.size(); }

    // Slots shown per action.
    static constexpr usize kSlots = 2;

private:
    struct Row {
        input::ActionId action;
        std::array<KeybindField*, kSlots> fields{};
        Button* reset = nullptr;
        Label* warning = nullptr;
    };

    [[nodiscard]] ScrollView& buildFrame();
    void buildPage();
    void addRow(Widget& page, input::ActionId action);
    [[nodiscard]] bool matchesFilter(const input::ActionInfo& info) const;
    void captured(input::ActionId action, usize slot, input::InputSystem::CaptureOutcome outcome,
                  const input::Binding& binding);
    void askAboutConflict();

    input::InputSystem& m_input;
    UiContext& m_context;
    std::string m_filter;
    ScrollView* m_scroll;
    std::vector<Row> m_rows;

    // The key waiting for the answer to the conflict dialog.
    struct PendingKey {
        input::ActionId action;
        usize slot = 0;
        input::Binding binding;
        std::vector<input::InputSystem::Conflict> conflicts;
    } m_pending{};
};

} // namespace vxe::ui
