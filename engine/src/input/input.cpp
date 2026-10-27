#include "vxe/input/input.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <variant>

namespace vxe::input {
namespace {

bool inRange(u16 scancode) noexcept {
    return scancode < scancode::kCount;
}

// Two bindings compete for the same physical input when kind and code match;
// modifiers and chords only narrow when a binding fires.
bool samePhysicalInput(const Binding& a, const Binding& b) noexcept {
    return a.kind == b.kind && a.code == b.code;
}

// The usual std::visit helper: one lambda per alternative. Inheriting from
// every lambda is the point of it.
template <typename... Fs>
struct Overloaded : Fs... { // NOLINT(misc-multiple-inheritance)
    using Fs::operator()...;
};

} // namespace

InputContextId InputSystem::registerContext(const ContextDesc& desc) {
    VXE_VERIFY(m_contexts.size() < 0xFF, "Too many input contexts");
    m_contexts.push_back(Context{.name = std::string(desc.name),
                                 .blocksLowerContexts = desc.blocksLowerContexts,
                                 .alwaysActive = desc.alwaysActive});
    return InputContextId{.index = static_cast<u8>(m_contexts.size() - 1)};
}

ActionId InputSystem::registerAction(const ActionDesc& desc) {
    VXE_ASSERT(desc.context.isValid() && desc.context.index < m_contexts.size(),
               "Action in an unknown context");
    if (const ActionId existing = findAction(desc.id); existing.isValid()) {
        return existing;
    }
    m_actions.push_back(ActionInfo{.id = std::string(desc.id),
                                   .displayName = std::string(desc.displayName),
                                   .category = std::string(desc.category),
                                   .context = desc.context,
                                   .type = desc.type,
                                   .defaults = desc.defaults,
                                   .bindings = desc.defaults});
    m_states.emplace_back();
    return ActionId{.index = static_cast<u32>(m_actions.size() - 1)};
}

const ActionInfo& InputSystem::action(ActionId id) const {
    VXE_ASSERT(id.isValid() && id.index < m_actions.size(), "Unknown action");
    return m_actions[id.index];
}

ActionId InputSystem::findAction(std::string_view id) const {
    for (usize i = 0; i < m_actions.size(); ++i) {
        if (m_actions[i].id == id) {
            return ActionId{.index = static_cast<u32>(i)};
        }
    }
    return {};
}

void InputSystem::setBinding(ActionId action, usize slot, const Binding& binding) {
    VXE_ASSERT(action.isValid() && action.index < m_actions.size() && slot < kMaxBindingsPerAction,
               "Invalid action or binding slot");
    m_actions[action.index].bindings[slot] = binding;
}

void InputSystem::pushContext(InputContextId context) {
    VXE_ASSERT(context.isValid() && context.index < m_contexts.size(), "Unknown input context");
    std::erase(m_stack, context);
    m_stack.push_back(context);
}

void InputSystem::popContext(InputContextId context) {
    std::erase(m_stack, context);
}

bool InputSystem::isContextActive(InputContextId context) const {
    return std::ranges::find(m_stack, context) != m_stack.end();
}

platform::Modifiers InputSystem::heldModifiers() const noexcept {
    using platform::Modifiers;
    Modifiers held = Modifiers::None;
    if (m_keys[scancode::kLeftShift] || m_keys[scancode::kRightShift]) {
        held = held | Modifiers::Shift;
    }
    if (m_keys[scancode::kLeftCtrl] || m_keys[scancode::kRightCtrl]) {
        held = held | Modifiers::Ctrl;
    }
    if (m_keys[scancode::kLeftAlt] || m_keys[scancode::kRightAlt]) {
        held = held | Modifiers::Alt;
    }
    if (m_keys[scancode::kLeftGui] || m_keys[scancode::kRightGui]) {
        held = held | Modifiers::Gui;
    }
    return held;
}

void InputSystem::applyEvent(const platform::PlatformEvent& event) {
    std::visit(Overloaded{
                   [this](const platform::KeyEvent& key) {
                       if (!inRange(key.scancode)) {
                           return;
                       }
                       if (key.down) {
                           // Repeats keep the key down but are not new presses.
                           if (!key.repeat && !m_keys[key.scancode]) {
                               m_keysPressedThisFrame.set(key.scancode);
                           }
                           m_keys.set(key.scancode);
                       } else {
                           m_keys.reset(key.scancode);
                           m_suppressedKeys.reset(key.scancode);
                       }
                   },
                   [this](const platform::MouseButtonEvent& button) {
                       const auto index = static_cast<usize>(button.button);
                       if (index >= m_mouseButtons.size()) {
                           return;
                       }
                       if (button.down && !m_mouseButtons[index]) {
                           m_mouseButtonsPressedThisFrame.set(index);
                       }
                       m_mouseButtons.set(index, button.down);
                       if (!button.down) {
                           m_suppressedButtons.reset(index);
                       }
                       m_mousePosition = Vec2(button.x, button.y);
                   },
                   [this](const platform::MouseMotionEvent& motion) {
                       m_mousePosition = Vec2(motion.x, motion.y);
                       m_mouseDelta += Vec2(motion.dx, motion.dy);
                   },
                   [this](const platform::MouseWheelEvent& wheel) { m_wheel += Vec2(wheel.x, wheel.y); },
                   [this](const platform::TextInputEvent& text) { m_typedText += text.text; },
                   [this](const platform::WindowFocusChanged& focus) {
                       // Keys released while another window had focus never
                       // send an "up" event; forgetting them avoids stuck keys.
                       if (!focus.focused) {
                           m_keys.reset();
                           m_mouseButtons.reset();
                           m_suppressedKeys.reset();
                           m_suppressedButtons.reset();
                       }
                   },
                   [](const auto& /*other*/) {},
               },
               event);
}

bool InputSystem::isBindingActive(const Binding& binding) const noexcept {
    if (!platform::hasAll(heldModifiers(), binding.modifiers)) {
        return false;
    }
    switch (binding.kind) {
    case BindingKind::None:
        return false;
    // "Down, or pressed during this frame": a quick tap that goes down and up
    // between two frames still counts for one frame instead of being lost.
    case BindingKind::Key:
        return inRange(binding.code) && !m_suppressedKeys[binding.code] &&
               (m_keys[binding.code] || m_keysPressedThisFrame[binding.code]) &&
               (binding.chordKey == 0 ||
                (inRange(binding.chordKey) &&
                 (m_keys[binding.chordKey] || m_keysPressedThisFrame[binding.chordKey])));
    case BindingKind::MouseButton:
        return binding.code < m_mouseButtons.size() && !m_suppressedButtons[binding.code] &&
               (m_mouseButtons[binding.code] || m_mouseButtonsPressedThisFrame[binding.code]);
    case BindingKind::WheelUp:
        return m_wheel.y > 0.0F;
    case BindingKind::WheelDown:
        return m_wheel.y < 0.0F;
    case BindingKind::WheelLeft:
        return m_wheel.x < 0.0F;
    case BindingKind::WheelRight:
        return m_wheel.x > 0.0F;
    }
    return false;
}

f32 InputSystem::bindingAxisValue(const Binding& binding) const noexcept {
    if (!isBindingActive(binding)) {
        return 0.0F;
    }
    switch (binding.kind) {
    case BindingKind::WheelUp:
    case BindingKind::WheelDown:
        return m_wheel.y; // Positive away from the user, negative towards.
    case BindingKind::WheelLeft:
    case BindingKind::WheelRight:
        return m_wheel.x;
    case BindingKind::None:
    case BindingKind::Key:
    case BindingKind::MouseButton:
        break;
    }
    return 1.0F; // A held key or button.
}

void InputSystem::update(std::span<const platform::PlatformEvent> events) {
    VXE_PROFILE_FUNCTION();
    m_keysPressedThisFrame.reset();
    m_mouseButtonsPressedThisFrame.reset();
    m_mouseDelta = Vec2(0.0F);
    m_wheel = Vec2(0.0F);
    m_typedText.clear();
    for (const platform::PlatformEvent& event : events) {
        applyEvent(event);
    }

    for (ActionState& state : m_states) {
        state.wasHeld = state.held;
        state.held = false;
        state.axisValue = 0.0F;
    }

    // While the keybind screen listens for a new input, nothing else may
    // react to the keys being pressed.
    if (m_capture) {
        processCapture(events);
        return;
    }

    // Walk the context stack from the top. A context claims the physical
    // inputs of its active actions, so contexts below never see them (the
    // menu's Escape is not also the game's pause). A blocking context hides
    // everything below it, except contexts that are always active.
    m_claimed.clear();
    bool blocked = false;
    for (auto contextIt = m_stack.rbegin(); contextIt != m_stack.rend(); ++contextIt) {
        const Context& context = m_contexts[contextIt->index];
        if (blocked && !context.alwaysActive) {
            continue;
        }
        const usize claimedByHigherContexts = m_claimed.size();
        for (usize i = 0; i < m_actions.size(); ++i) {
            const ActionInfo& info = m_actions[i];
            if (info.context != *contextIt) {
                continue;
            }
            ActionState& state = m_states[i];
            for (const Binding& binding : info.bindings) {
                const bool claimedAbove = std::any_of(
                    m_claimed.begin(),
                    m_claimed.begin() + static_cast<std::ptrdiff_t>(claimedByHigherContexts),
                    [&binding](const Binding& claimed) { return samePhysicalInput(claimed, binding); });
                if (claimedAbove || !isBindingActive(binding)) {
                    continue;
                }
                state.held = true;
                state.axisValue += bindingAxisValue(binding);
                m_claimed.push_back(binding);
            }
        }
        if (context.blocksLowerContexts) {
            blocked = true;
        }
    }

    for (usize i = 0; i < m_states.size(); ++i) {
        ActionState& state = m_states[i];
        if (m_actions[i].type == ActionType::Toggle && state.held && !state.wasHeld) {
            state.toggledOn = !state.toggledOn;
        }
    }
}

void InputSystem::resetBindings(ActionId action) {
    VXE_ASSERT(action.isValid() && action.index < m_actions.size(), "Unknown action");
    m_actions[action.index].bindings = m_actions[action.index].defaults;
}

usize InputSystem::resetCategory(std::string_view category) {
    usize count = 0;
    for (ActionInfo& info : m_actions) {
        if (info.category == category) {
            info.bindings = info.defaults;
            ++count;
        }
    }
    return count;
}

void InputSystem::resetAllBindings() {
    for (ActionInfo& info : m_actions) {
        info.bindings = info.defaults;
    }
}

std::vector<InputSystem::Conflict> InputSystem::findConflicts(const Binding& binding, InputContextId context,
                                                              ActionId ignore) const {
    std::vector<Conflict> conflicts;
    if (binding.isNone() || !context.isValid() || context.index >= m_contexts.size()) {
        return conflicts;
    }
    const bool contextAlwaysActive = m_contexts[context.index].alwaysActive;
    for (usize i = 0; i < m_actions.size(); ++i) {
        const ActionInfo& other = m_actions[i];
        if (ignore.isValid() && ignore.index == i) {
            continue;
        }
        const bool overlapping =
            other.context == context || contextAlwaysActive || m_contexts[other.context.index].alwaysActive;
        if (!overlapping) {
            continue;
        }
        for (usize slot = 0; slot < kMaxBindingsPerAction; ++slot) {
            if (!other.bindings[slot].isNone() && samePhysicalInput(other.bindings[slot], binding)) {
                conflicts.push_back(Conflict{.action = ActionId{.index = static_cast<u32>(i)}, .slot = slot});
            }
        }
    }
    return conflicts;
}

void InputSystem::swapBindings(ActionId a, usize slotA, ActionId b, usize slotB) {
    VXE_ASSERT(a.isValid() && b.isValid() && a.index < m_actions.size() && b.index < m_actions.size() &&
                   slotA < kMaxBindingsPerAction && slotB < kMaxBindingsPerAction,
               "Invalid swap");
    std::swap(m_actions[a.index].bindings[slotA], m_actions[b.index].bindings[slotB]);
}

void InputSystem::setUnknownKeybind(std::string id, std::string tomlValueText) {
    const auto position =
        std::ranges::lower_bound(m_unknownKeybinds, id, {}, &std::pair<std::string, std::string>::first);
    if (position != m_unknownKeybinds.end() && position->first == id) {
        position->second = std::move(tomlValueText);
    } else {
        m_unknownKeybinds.insert(position, {std::move(id), std::move(tomlValueText)});
    }
}

void InputSystem::beginCapture(CaptureCallback onCaptured) {
    m_capture = std::move(onCaptured);
    m_captureModifierKey = 0;
}

void InputSystem::cancelCapture() {
    if (m_capture) {
        finishCapture(CaptureOutcome::Cancelled, Binding{});
    }
}

void InputSystem::finishCapture(CaptureOutcome outcome, const Binding& binding) {
    // Moved out first: the callback may start the next capture right away.
    const CaptureCallback callback = std::move(m_capture);
    m_capture = nullptr;
    m_captureModifierKey = 0;
    if (callback) {
        callback(outcome, binding);
    }
}

namespace {

bool isModifierKey(u16 code) noexcept {
    return code >= scancode::kLeftCtrl && code <= scancode::kRightGui;
}

BindingKind wheelDirection(const platform::MouseWheelEvent& wheel) noexcept {
    if (wheel.y > 0.0F) {
        return BindingKind::WheelUp;
    }
    if (wheel.y < 0.0F) {
        return BindingKind::WheelDown;
    }
    if (wheel.x > 0.0F) {
        return BindingKind::WheelRight;
    }
    if (wheel.x < 0.0F) {
        return BindingKind::WheelLeft;
    }
    return BindingKind::None;
}

} // namespace

void InputSystem::processCaptureKey(const platform::KeyEvent& key) {
    if (!inRange(key.scancode)) {
        return;
    }
    if (key.down && !key.repeat) {
        if (key.scancode == scancode::kEscape || key.scancode == scancode::kBackspace) {
            m_suppressedKeys.set(key.scancode);
            finishCapture(key.scancode == scancode::kEscape ? CaptureOutcome::Cancelled
                                                            : CaptureOutcome::Cleared,
                          Binding{});
        } else if (isModifierKey(key.scancode)) {
            // Wait: this may become Ctrl+S, or Left Shift on its own.
            if (m_captureModifierKey == 0) {
                m_captureModifierKey = key.scancode;
            }
        } else {
            m_suppressedKeys.set(key.scancode);
            finishCapture(CaptureOutcome::Captured, Binding::key(key.scancode, heldModifiers()));
        }
    } else if (!key.down && key.scancode == m_captureModifierKey) {
        finishCapture(CaptureOutcome::Captured, Binding::key(key.scancode));
    }
}

void InputSystem::processCapture(std::span<const platform::PlatformEvent> events) {
    for (const platform::PlatformEvent& event : events) {
        if (!m_capture) {
            return;
        }
        if (const auto* key = std::get_if<platform::KeyEvent>(&event)) {
            processCaptureKey(*key);
        } else if (const auto* button = std::get_if<platform::MouseButtonEvent>(&event)) {
            if (!button->down) {
                continue;
            }
            const auto index = static_cast<usize>(button->button);
            if (index < m_suppressedButtons.size()) {
                m_suppressedButtons.set(index);
            }
            Binding binding = Binding::mouse(button->button);
            binding.modifiers = heldModifiers();
            finishCapture(CaptureOutcome::Captured, binding);
        } else if (const auto* wheel = std::get_if<platform::MouseWheelEvent>(&event)) {
            if (const BindingKind direction = wheelDirection(*wheel); direction != BindingKind::None) {
                finishCapture(CaptureOutcome::Captured, Binding::wheel(direction));
            }
        }
    }
}

bool InputSystem::isPressed(ActionId action) const noexcept {
    const ActionState& state = m_states[action.index];
    return state.held && !state.wasHeld;
}

bool InputSystem::isHeld(ActionId action) const noexcept {
    return m_states[action.index].held;
}

bool InputSystem::isReleased(ActionId action) const noexcept {
    const ActionState& state = m_states[action.index];
    return !state.held && state.wasHeld;
}

bool InputSystem::isToggledOn(ActionId action) const noexcept {
    return m_states[action.index].toggledOn;
}

f32 InputSystem::axis(ActionId action) const noexcept {
    return m_states[action.index].axisValue;
}

bool InputSystem::isKeyDown(u16 scancode) const noexcept {
    return inRange(scancode) && m_keys[scancode];
}

bool InputSystem::wasKeyPressed(u16 scancode) const noexcept {
    return inRange(scancode) && m_keysPressedThisFrame[scancode];
}

bool InputSystem::isMouseButtonDown(platform::MouseButton button) const noexcept {
    const auto index = static_cast<usize>(button);
    return index < m_mouseButtons.size() && m_mouseButtons[index];
}

} // namespace vxe::input
