#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/input/input.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Keybind persistence (Section 17.2 and 17.3, ADR-014).
//
// Bindings are written as readable text, so keybinds.toml can be edited by
// hand:
//   "W"  "Space"  "Left Shift"   physical keys (layout independent names)
//   "Ctrl+S"  "Shift+Alt+F"      with required modifiers
//   "F3+G"                       a chord: hold F3, press G
//   "Mouse Left"  "Mouse X1"     mouse buttons
//   "Wheel Up"                   wheel directions
//   "key:Z"                      the key labeled Z in the current layout
//   ""                           no binding
//
// By default keys are stored by physical position, so WASD stays under the
// left hand on any layout. With Controls.BindByKeyName on, they are stored by
// label ("key:Z") and resolved to the key that types that label when loaded.
namespace vxe::input {

enum class KeyNameMode : u8 {
    Physical, // Scancode names. The default.
    Layout,   // Key labels in the current keyboard layout.
};

[[nodiscard]] std::string bindingToString(const Binding& binding, KeyNameMode mode = KeyNameMode::Physical);
[[nodiscard]] Result<Binding> bindingFromString(std::string_view input);
// A binding as the player reads it in the keybind screen: keys by their label
// in the current keyboard layout ("Ctrl+Z"), empty for no binding. For display
// only; files use bindingToString.
[[nodiscard]] std::string bindingLabel(const Binding& binding);

struct KeybindLoadReport {
    usize applied = 0; // Actions whose bindings were set.
    std::vector<std::string> warnings;
};

// Applies a keybinds TOML text. Fails only when the text is not valid TOML;
// bad entries become warnings and keep the current binding.
[[nodiscard]] Result<KeybindLoadReport> loadKeybindsFromString(InputSystem& input, std::string_view toml);
// NotFound when the file does not exist yet (first start).
[[nodiscard]] Result<KeybindLoadReport> loadKeybindsFromFile(InputSystem& input,
                                                             const std::filesystem::path& path);

[[nodiscard]] std::string saveKeybindsToString(const InputSystem& input,
                                               KeyNameMode mode = KeyNameMode::Physical);
[[nodiscard]] Result<void> saveKeybindsToFile(const InputSystem& input, const std::filesystem::path& path,
                                              KeyNameMode mode = KeyNameMode::Physical);

// Registers the input settings (Controls.BindByKeyName) and returns the
// setting that chooses the key name mode for saving.
[[nodiscard]] config::Setting<bool> registerInputSettings(config::SettingsRegistry& settings);

} // namespace vxe::input
