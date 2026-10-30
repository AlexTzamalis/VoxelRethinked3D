#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/platform/events.hpp>

#include <array>
#include <bitset>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Input: raw device state and actions (Section 17 of the plan).
//
// The game never asks "is W pressed". It asks "is the action move_forward
// active". Actions are registered with default bindings, live in contexts
// (Menu, Gameplay, Console, ...), and the context stack decides which action
// gets a physical input when several want it.
namespace vxe::input {

// Well known scancodes (USB HID usage ids, the same as SDL_Scancode). Only
// the ones the engine itself refers to; games use their own or the numbers.
namespace scancode {
inline constexpr u16 kA = 4;
inline constexpr u16 kD = 7;
inline constexpr u16 kE = 8;
inline constexpr u16 kG = 10;
inline constexpr u16 kS = 22;
inline constexpr u16 kT = 23;
inline constexpr u16 kW = 26;
inline constexpr u16 k1 = 30;
inline constexpr u16 kReturn = 40;
inline constexpr u16 kEscape = 41;
inline constexpr u16 kBackspace = 42;
inline constexpr u16 kTab = 43;
inline constexpr u16 kSpace = 44;
inline constexpr u16 kGrave = 53;
inline constexpr u16 kF1 = 58;
inline constexpr u16 kF2 = 59;
inline constexpr u16 kF3 = 60;
inline constexpr u16 kF6 = 63;
inline constexpr u16 kF11 = 68;
inline constexpr u16 kHome = 74;
inline constexpr u16 kDelete = 76;
inline constexpr u16 kEnd = 77;
inline constexpr u16 kRight = 79;
inline constexpr u16 kLeft = 80;
inline constexpr u16 kDown = 81;
inline constexpr u16 kUp = 82;
inline constexpr u16 kKeypadEnter = 88;
inline constexpr u16 kLeftCtrl = 224;
inline constexpr u16 kLeftShift = 225;
inline constexpr u16 kLeftAlt = 226;
inline constexpr u16 kLeftGui = 227;
inline constexpr u16 kRightCtrl = 228;
inline constexpr u16 kRightShift = 229;
inline constexpr u16 kRightAlt = 230;
inline constexpr u16 kRightGui = 231;
inline constexpr u16 kCount = 512;
} // namespace scancode

enum class BindingKind : u8 {
    None,
    Key,         // `code` is a scancode.
    MouseButton, // `code` is a platform::MouseButton value.
    WheelUp,     // One notch away from the user.
    WheelDown,
    WheelLeft,
    WheelRight,
};

// One physical input that can trigger an action.
struct Binding {
    BindingKind kind = BindingKind::None;
    u16 code = 0;
    // Modifiers that must be held (Ctrl+S). Extra held modifiers are allowed,
    // so holding Ctrl to sprint does not stop W from moving forward.
    platform::Modifiers modifiers = platform::Modifiers::None;
    // A second key that must be held, for debug chords like F3+G
    // (Section 17.5). 0 for none.
    u16 chordKey = 0;

    [[nodiscard]] static constexpr Binding
    key(u16 scancode, platform::Modifiers held = platform::Modifiers::None) noexcept {
        return {.kind = BindingKind::Key, .code = scancode, .modifiers = held, .chordKey = 0};
    }
    [[nodiscard]] static constexpr Binding chord(u16 holdKey, u16 pressKey) noexcept {
        return {.kind = BindingKind::Key,
                .code = pressKey,
                .modifiers = platform::Modifiers::None,
                .chordKey = holdKey};
    }
    [[nodiscard]] static constexpr Binding mouse(platform::MouseButton button) noexcept {
        return {.kind = BindingKind::MouseButton,
                .code = static_cast<u16>(button),
                .modifiers = platform::Modifiers::None,
                .chordKey = 0};
    }
    [[nodiscard]] static constexpr Binding wheel(BindingKind direction) noexcept {
        return {.kind = direction, .code = 0, .modifiers = platform::Modifiers::None, .chordKey = 0};
    }

    [[nodiscard]] constexpr bool isNone() const noexcept { return kind == BindingKind::None; }
    friend constexpr bool operator==(const Binding&, const Binding&) = default;
};

enum class ActionType : u8 {
    Button, // Pressed, held, released.
    Toggle, // Flips between on and off on every press.
    Axis,   // A value: wheel notches this frame, or 1 while a bound key is held.
};

// Identifies a registered input context. Small and cheap to copy.
struct InputContextId {
    u8 index = 0xFF;
    [[nodiscard]] constexpr bool isValid() const noexcept { return index != 0xFF; }
    friend constexpr bool operator==(const InputContextId&, const InputContextId&) = default;
};

// Identifies a registered action.
struct ActionId {
    u32 index = 0xFFFFFFFF;
    [[nodiscard]] constexpr bool isValid() const noexcept { return index != 0xFFFFFFFF; }
    friend constexpr bool operator==(const ActionId&, const ActionId&) = default;
};

inline constexpr usize kMaxBindingsPerAction = 3; // Primary, secondary, tertiary (Section 17.2).

struct ContextDesc {
    std::string_view name;
    // Hides every input from the contexts below while this one is on the
    // stack (the console takes all typing, a menu takes all clicks).
    bool blocksLowerContexts = false;
    // Never blocked by a higher context (Global: fullscreen, screenshot).
    bool alwaysActive = false;
};

// Describes an action the player can perform. The game registers these at
// startup with default bindings (Section 17.4).
struct ActionDesc {
    std::string_view id;          // Stable, saved in keybinds.toml, for example "gameplay.jump".
    std::string_view displayName; // "Jump".
    std::string_view category;    // Groups rows in the keybind screen, "Movement".
    InputContextId context;
    ActionType type = ActionType::Button;
    std::array<Binding, kMaxBindingsPerAction> defaults{};
};

struct ActionInfo {
    std::string id;
    std::string displayName;
    std::string category;
    InputContextId context;
    ActionType type = ActionType::Button;
    std::array<Binding, kMaxBindingsPerAction> defaults{};
    std::array<Binding, kMaxBindingsPerAction> bindings{}; // Current bindings.
};

// Raw device state plus actions, updated once per frame on the main thread.
//
// Thread safety: main thread only.
class InputSystem {
public:
    InputContextId registerContext(const ContextDesc& desc);
    ActionId registerAction(const ActionDesc& desc);

    // Folds this frame's OS events into the raw state and recomputes every
    // action. Call once per frame, before game code reads input.
    void update(std::span<const platform::PlatformEvent> events);

    // Context stack. Pushing a context that is already on the stack moves it
    // to the top; popping one that is not there does nothing.
    void pushContext(InputContextId context);
    void popContext(InputContextId context);
    [[nodiscard]] bool isContextActive(InputContextId context) const;
    [[nodiscard]] std::span<const InputContextId> contextStack() const noexcept { return m_stack; }
    // The name a context was registered with, for debug views. Empty for
    // unknown ids.
    [[nodiscard]] std::string_view contextName(InputContextId context) const noexcept;

    [[nodiscard]] bool isPressed(ActionId action) const noexcept;  // Only on the frame it went down.
    [[nodiscard]] bool isHeld(ActionId action) const noexcept;     // Every frame while down.
    [[nodiscard]] bool isReleased(ActionId action) const noexcept; // Only on the frame it went up.
    [[nodiscard]] bool isToggledOn(ActionId action) const noexcept;
    [[nodiscard]] f32 axis(ActionId action) const noexcept;

    // Raw state, for code that genuinely needs devices (UI text fields,
    // mouse look). Not filtered by contexts.
    [[nodiscard]] bool isKeyDown(u16 scancode) const noexcept;
    // Went down this frame or repeated while held (the OS key repeat): for
    // text editing, where holding Backspace keeps deleting.
    [[nodiscard]] bool wasKeyTyped(u16 scancode) const noexcept;
    [[nodiscard]] bool wasKeyPressed(u16 scancode) const noexcept;
    [[nodiscard]] bool isMouseButtonDown(platform::MouseButton button) const noexcept;
    [[nodiscard]] Vec2 mousePosition() const noexcept { return m_mousePosition; }
    [[nodiscard]] Vec2 mouseDelta() const noexcept { return m_mouseDelta; } // Sum of this frame's motion.
    [[nodiscard]] Vec2 wheelDelta() const noexcept { return m_wheel; }
    [[nodiscard]] const std::string& typedText() const noexcept { return m_typedText; }
    [[nodiscard]] platform::Modifiers heldModifiers() const noexcept;

    [[nodiscard]] const ActionInfo& action(ActionId id) const;
    [[nodiscard]] ActionId findAction(std::string_view id) const;
    [[nodiscard]] usize actionCount() const noexcept { return m_actions.size(); }

    // Replaces one binding slot (0 to 2) of an action.
    void setBinding(ActionId action, usize slot, const Binding& binding);
    void clearBinding(ActionId action, usize slot) { setBinding(action, slot, Binding{}); }

    // Restores default bindings (Section 17.3: "Reset this action", "Reset
    // category", "Reset all").
    void resetBindings(ActionId action);
    usize resetCategory(std::string_view category);
    void resetAllBindings();

    // One binding slot of another action that uses the same physical input.
    struct Conflict {
        ActionId action;
        usize slot = 0;
        friend bool operator==(const Conflict&, const Conflict&) = default;
    };

    // The bindings that would fight with `binding` in `context`: same
    // physical input, in the same context or in an always active one (those
    // see input whatever is on the stack). Actions in different stacked
    // contexts do not conflict, because the context stack already decides
    // who gets the input. `ignore` is usually the action being rebound.
    [[nodiscard]] std::vector<Conflict> findConflicts(const Binding& binding, InputContextId context,
                                                      ActionId ignore = {}) const;

    // Swaps one slot of two actions, for "Swap" in the conflict dialog.
    void swapBindings(ActionId a, usize slotA, ActionId b, usize slotB);

    enum class CaptureOutcome : u8 {
        Captured,  // `binding` holds the new input.
        Cancelled, // Escape: keep the old binding.
        Cleared,   // Backspace: remove the binding.
    };
    using CaptureCallback = std::function<void(CaptureOutcome outcome, const Binding& binding)>;

    // Listens for the next physical input and reports it through `onCaptured`
    // instead of triggering actions (the keybind screen, Section 17.3). A key
    // is captured with the modifiers held at the time; a modifier pressed and
    // released alone is captured as itself (Left Shift for crouch). The
    // captured input does not trigger any action until it is released.
    void beginCapture(CaptureCallback onCaptured);
    void cancelCapture();
    [[nodiscard]] bool isCapturing() const noexcept { return static_cast<bool>(m_capture); }

    // Keybinds from a file that no registered action claims (older or newer
    // versions). Kept as TOML value text so saving never deletes them.
    void setUnknownKeybind(std::string id, std::string tomlValueText);
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& unknownKeybinds() const noexcept {
        return m_unknownKeybinds;
    }

private:
    struct Context {
        std::string name;
        bool blocksLowerContexts = false;
        bool alwaysActive = false;
    };

    struct ActionState {
        bool held = false;
        bool wasHeld = false;
        // One of its inputs went down this frame. Not simply "held and not
        // held last frame": an action whose context comes back while its
        // key is still down (Escape closing a menu over the game) must not
        // see that key as a new press.
        bool pressed = false;
        bool toggledOn = false;
        f32 axisValue = 0.0F;
    };

    void applyEvent(const platform::PlatformEvent& event);
    [[nodiscard]] bool isBindingActive(const Binding& binding) const noexcept;
    [[nodiscard]] bool wentDownThisFrame(const Binding& binding) const noexcept;
    [[nodiscard]] f32 bindingAxisValue(const Binding& binding) const noexcept;

    std::vector<Context> m_contexts;
    std::vector<InputContextId> m_stack; // Bottom first.
    std::vector<ActionInfo> m_actions;
    std::vector<ActionState> m_states;

    std::bitset<scancode::kCount> m_keys;
    std::bitset<scancode::kCount> m_keysPressedThisFrame;
    std::bitset<scancode::kCount> m_keysTypedThisFrame;
    std::bitset<8> m_mouseButtons;
    std::bitset<8> m_mouseButtonsPressedThisFrame;
    Vec2 m_mousePosition{0.0F};
    Vec2 m_mouseDelta{0.0F};
    Vec2 m_wheel{0.0F};
    std::string m_typedText;
    std::vector<Binding> m_claimed; // Reused by update() to avoid reallocating.

    // Capture state. Inputs used to finish a capture stay suppressed until
    // released, so the new binding does not fire right away.
    void processCapture(std::span<const platform::PlatformEvent> events);
    void processCaptureKey(const platform::KeyEvent& key);
    void finishCapture(CaptureOutcome outcome, const Binding& binding);
    CaptureCallback m_capture;
    u16 m_captureModifierKey = 0;
    std::bitset<scancode::kCount> m_suppressedKeys;
    std::bitset<8> m_suppressedButtons;

    std::vector<std::pair<std::string, std::string>> m_unknownKeybinds; // Sorted by id.
};

} // namespace vxe::input
