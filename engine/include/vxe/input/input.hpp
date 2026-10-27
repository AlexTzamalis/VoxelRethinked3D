#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/platform/events.hpp>

#include <array>
#include <bitset>
#include <span>
#include <string>
#include <string_view>
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
inline constexpr u16 kRight = 79;
inline constexpr u16 kLeft = 80;
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

    [[nodiscard]] bool isPressed(ActionId action) const noexcept;  // Only on the frame it went down.
    [[nodiscard]] bool isHeld(ActionId action) const noexcept;     // Every frame while down.
    [[nodiscard]] bool isReleased(ActionId action) const noexcept; // Only on the frame it went up.
    [[nodiscard]] bool isToggledOn(ActionId action) const noexcept;
    [[nodiscard]] f32 axis(ActionId action) const noexcept;

    // Raw state, for code that genuinely needs devices (UI text fields,
    // mouse look). Not filtered by contexts.
    [[nodiscard]] bool isKeyDown(u16 scancode) const noexcept;
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

    // Replaces one binding slot (0 to 2) of an action. E5.5 builds rebinding,
    // persistence and conflict checks on top.
    void setBinding(ActionId action, usize slot, const Binding& binding);

private:
    struct Context {
        std::string name;
        bool blocksLowerContexts = false;
        bool alwaysActive = false;
    };

    struct ActionState {
        bool held = false;
        bool wasHeld = false;
        bool toggledOn = false;
        f32 axisValue = 0.0F;
    };

    void applyEvent(const platform::PlatformEvent& event);
    [[nodiscard]] bool isBindingActive(const Binding& binding) const noexcept;
    [[nodiscard]] f32 bindingAxisValue(const Binding& binding) const noexcept;

    std::vector<Context> m_contexts;
    std::vector<InputContextId> m_stack; // Bottom first.
    std::vector<ActionInfo> m_actions;
    std::vector<ActionState> m_states;

    std::bitset<scancode::kCount> m_keys;
    std::bitset<scancode::kCount> m_keysPressedThisFrame;
    std::bitset<8> m_mouseButtons;
    std::bitset<8> m_mouseButtonsPressedThisFrame;
    Vec2 m_mousePosition{0.0F};
    Vec2 m_mouseDelta{0.0F};
    Vec2 m_wheel{0.0F};
    std::string m_typedText;
    std::vector<Binding> m_claimed; // Reused by update() to avoid reallocating.
};

} // namespace vxe::input
