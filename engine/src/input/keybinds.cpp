#include "vxe/input/keybinds.hpp"

#include <vxe/io/file_io.hpp>
#include <vxe/platform/platform.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <map>
#include <sstream>

namespace vxe::input {
namespace {

using platform::Modifiers;

struct NamedModifier {
    Modifiers modifier;
    std::string_view name;
};
constexpr std::array<NamedModifier, 4> kModifierNames{{
    {.modifier = Modifiers::Ctrl, .name = "Ctrl"},
    {.modifier = Modifiers::Shift, .name = "Shift"},
    {.modifier = Modifiers::Alt, .name = "Alt"},
    {.modifier = Modifiers::Gui, .name = "Gui"},
}};

constexpr std::array<std::string_view, 6> kMouseNames{"", "Left", "Middle", "Right", "X1", "X2"};

struct NamedWheel {
    BindingKind kind;
    std::string_view name;
};
constexpr std::array<NamedWheel, 4> kWheelNames{{
    {.kind = BindingKind::WheelUp, .name = "Up"},
    {.kind = BindingKind::WheelDown, .name = "Down"},
    {.kind = BindingKind::WheelLeft, .name = "Left"},
    {.kind = BindingKind::WheelRight, .name = "Right"},
}};

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && startsWithIgnoreCase(a, b);
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::string keyToString(u16 scancode, KeyNameMode mode) {
    if (mode == KeyNameMode::Layout) {
        const std::string label = platform::Platform::layoutKeyName(scancode);
        if (!label.empty()) {
            return "key:" + label;
        }
    }
    const std::string name = platform::Platform::scancodeName(scancode);
    // Keys without a name still round trip by number.
    return name.empty() ? std::format("Scancode {}", scancode) : name;
}

// 0 when the text names no key.
u16 parseKey(std::string_view text) {
    if (startsWithIgnoreCase(text, "key:")) {
        return platform::Platform::scancodeFromKeyName(text.substr(4));
    }
    if (startsWithIgnoreCase(text, "Scancode ")) {
        u16 code = 0;
        const std::string_view digits = text.substr(9);
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), code);
        return result.ec == std::errc{} && code < scancode::kCount ? code : u16{0};
    }
    return platform::Platform::scancodeFromName(text);
}

std::string tomlString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

} // namespace

std::string bindingLabel(const Binding& binding) {
    if (binding.kind != BindingKind::Key) {
        return bindingToString(binding);
    }
    const auto keyLabel = [](u16 scancode) {
        const std::string label = platform::Platform::layoutKeyName(scancode);
        return label.empty() ? keyToString(scancode, KeyNameMode::Physical) : label;
    };
    std::string text;
    for (const NamedModifier& named : kModifierNames) {
        if (platform::hasAll(binding.modifiers, named.modifier)) {
            text += std::format("{}+", named.name);
        }
    }
    if (binding.chordKey != 0) {
        text += keyLabel(binding.chordKey) + "+";
    }
    return text + keyLabel(binding.code);
}

std::string bindingToString(const Binding& binding, KeyNameMode mode) {
    if (binding.isNone()) {
        return {};
    }
    std::string text;
    for (const NamedModifier& named : kModifierNames) {
        if (platform::hasAll(binding.modifiers, named.modifier)) {
            text += std::format("{}+", named.name);
        }
    }
    switch (binding.kind) {
    case BindingKind::None:
        return {};
    case BindingKind::Key:
        if (binding.chordKey != 0) {
            text += keyToString(binding.chordKey, mode) + "+";
        }
        text += keyToString(binding.code, mode);
        return text;
    case BindingKind::MouseButton:
        return text +
               std::format("Mouse {}", binding.code < kMouseNames.size() ? kMouseNames[binding.code] : "?");
    case BindingKind::WheelUp:
    case BindingKind::WheelDown:
    case BindingKind::WheelLeft:
    case BindingKind::WheelRight:
        for (const NamedWheel& wheel : kWheelNames) {
            if (wheel.kind == binding.kind) {
                return text + std::format("Wheel {}", wheel.name);
            }
        }
        break;
    }
    return text;
}

Result<Binding> bindingFromString(std::string_view input) {
    std::string_view text = trim(input);
    if (text.empty()) {
        return Binding{};
    }
    const auto invalid = [&input] {
        return Error(ErrorCode::InvalidArgument,
                     std::format("'{}' is not a key, mouse button or wheel direction", input));
    };

    Binding binding;
    // Modifier prefixes, in any order and any case: "ctrl+shift+s".
    for (bool found = true; found;) {
        found = false;
        for (const NamedModifier& named : kModifierNames) {
            if (startsWithIgnoreCase(text, named.name) && text.size() > named.name.size() &&
                text[named.name.size()] == '+') {
                binding.modifiers = binding.modifiers | named.modifier;
                text.remove_prefix(named.name.size() + 1);
                found = true;
            }
        }
    }

    if (startsWithIgnoreCase(text, "Mouse ")) {
        const std::string_view button = text.substr(6);
        for (usize i = 1; i < kMouseNames.size(); ++i) {
            if (equalsIgnoreCase(button, kMouseNames[i])) {
                binding.kind = BindingKind::MouseButton;
                binding.code = static_cast<u16>(i);
                return binding;
            }
        }
        return invalid();
    }
    if (startsWithIgnoreCase(text, "Wheel ")) {
        const std::string_view direction = text.substr(6);
        for (const NamedWheel& wheel : kWheelNames) {
            if (equalsIgnoreCase(direction, wheel.name)) {
                binding.kind = wheel.kind;
                return binding;
            }
        }
        return invalid();
    }

    // A whole key name first, because names such as "Keypad +" contain a
    // plus; only then a chord split at the first plus ("F3+G").
    if (const u16 key = parseKey(text); key != 0) {
        binding.kind = BindingKind::Key;
        binding.code = key;
        return binding;
    }
    const usize plus = text.find('+');
    if (plus != std::string_view::npos && plus > 0 && plus + 1 < text.size()) {
        const u16 chord = parseKey(text.substr(0, plus));
        const u16 key = parseKey(text.substr(plus + 1));
        if (chord != 0 && key != 0) {
            binding.kind = BindingKind::Key;
            binding.code = key;
            binding.chordKey = chord;
            return binding;
        }
    }
    return invalid();
}

namespace {

// Applies one action's bindings from a TOML value: a string or an array of
// up to three strings. Slots not listed are cleared; bad entries keep the
// current binding of their slot and add a warning.
void applyActionValue(InputSystem& input, ActionId action, const std::string& id, const toml::node& node,
                      KeybindLoadReport& report) {
    std::vector<const toml::node*> entries;
    if (const toml::array* array = node.as_array()) {
        for (const toml::node& element : *array) {
            entries.push_back(&element);
        }
    } else {
        entries.push_back(&node);
    }
    if (entries.size() > kMaxBindingsPerAction) {
        report.warnings.push_back(
            std::format("'{}' has more than {} bindings; extra ones ignored", id, kMaxBindingsPerAction));
        entries.resize(kMaxBindingsPerAction);
    }
    for (usize slot = 0; slot < kMaxBindingsPerAction; ++slot) {
        if (slot >= entries.size()) {
            input.clearBinding(action, slot);
            continue;
        }
        const toml::node& entry = *entries[slot];
        if (!entry.is_string()) {
            report.warnings.push_back(std::format("'{}' binding {} must be text in quotes", id, slot + 1));
            continue;
        }
        Result<Binding> binding = bindingFromString(entry.as_string()->get());
        if (!binding) {
            report.warnings.push_back(
                std::format("'{}': {}; kept the current binding", id, binding.error().message()));
            continue;
        }
        input.setBinding(action, slot, binding.value());
    }
    ++report.applied;
}

std::string nodeToText(const toml::node& node) {
    std::ostringstream out;
    node.visit([&out](const auto& value) { out << toml::toml_formatter(value); });
    return out.str();
}

// The recursion is as deep as the table nesting of the file, a few levels.
// NOLINTNEXTLINE(misc-no-recursion)
void walk(const toml::table& table, const std::string& prefix, InputSystem& input,
          KeybindLoadReport& report) {
    for (const auto& [key, node] : table) {
        const std::string id =
            prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        if (const toml::table* nested = node.as_table()) {
            walk(*nested, id, input, report);
            continue;
        }
        const ActionId action = input.findAction(id);
        if (!action.isValid()) {
            input.setUnknownKeybind(id, nodeToText(node));
            continue;
        }
        applyActionValue(input, action, id, node, report);
    }
}

} // namespace

Result<KeybindLoadReport> loadKeybindsFromString(InputSystem& input, std::string_view toml) {
    toml::table root;
    // toml++ reports parse errors by exception; the engine does not throw
    // (Section 23.4), so it becomes an error here.
    try {
        root = toml::parse(toml);
    } catch (const toml::parse_error& error) {
        const auto& where = error.source().begin;
        return Error(ErrorCode::ParseError,
                     std::format("Keybinds file is not valid TOML (line {}, column {}): {}", where.line,
                                 where.column, error.description()));
    }
    KeybindLoadReport report;
    walk(root, std::string(), input, report);
    return report;
}

Result<KeybindLoadReport> loadKeybindsFromFile(InputSystem& input, const std::filesystem::path& path) {
    Result<std::vector<std::byte>> bytes = io::readFile(path);
    if (!bytes) {
        return std::move(bytes).error();
    }
    const std::vector<std::byte>& data = bytes.value();
    return loadKeybindsFromString(input,
                                  std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::string saveKeybindsToString(const InputSystem& input, KeyNameMode mode) {
    std::map<std::string, std::string> sections; // Sorted, so the file is stable.
    for (usize i = 0; i < input.actionCount(); ++i) {
        const ActionInfo& info = input.action(ActionId{.index = static_cast<u32>(i)});
        const usize dot = info.id.rfind('.');
        const std::string table = dot == std::string::npos ? std::string() : info.id.substr(0, dot);
        const std::string key = dot == std::string::npos ? info.id : info.id.substr(dot + 1);

        std::string defaults;
        for (const Binding& binding : info.defaults) {
            if (!binding.isNone()) {
                defaults += defaults.empty() ? "" : ", ";
                defaults += bindingToString(binding, KeyNameMode::Physical);
            }
        }
        std::string values;
        for (const Binding& binding : info.bindings) {
            values += values.empty() ? "" : ", ";
            values += tomlString(bindingToString(binding, mode));
        }
        std::string& section = sections[table];
        section += std::format("# {} ({}). Default: {}\n{} = [{}]\n", info.displayName, info.category,
                               defaults.empty() ? "none" : defaults, key, values);
    }
    for (const auto& [id, text] : input.unknownKeybinds()) {
        const usize dot = id.rfind('.');
        const std::string table = dot == std::string::npos ? std::string() : id.substr(0, dot);
        sections[table] +=
            std::format("# Not used by this version of the game; kept so other versions keep it.\n{} = {}\n",
                        dot == std::string::npos ? id : id.substr(dot + 1), text);
    }

    std::string out =
        "# Voxel Rethinked keybinds. Up to three bindings per action, \"\" for none.\n"
        "# Examples: \"W\", \"Ctrl+S\", \"F3+G\", \"Mouse Left\", \"Wheel Up\", \"key:Z\" (a key label\n"
        "# in your keyboard layout). The in game Controls screen edits this file too.\n";
    if (const auto top = sections.find(std::string()); top != sections.end()) {
        out += '\n' + top->second;
        sections.erase(top);
    }
    for (const auto& [table, lines] : sections) {
        out += std::format("\n[{}]\n{}", table, lines);
    }
    return out;
}

Result<void> saveKeybindsToFile(const InputSystem& input, const std::filesystem::path& path,
                                KeyNameMode mode) {
    const std::string text = saveKeybindsToString(input, mode);
    return io::writeFile(path, std::as_bytes(std::span(text.data(), text.size())));
}

config::Setting<bool> registerInputSettings(config::SettingsRegistry& settings) {
    config::BoolSettingDesc desc;
    desc.id = "Controls.BindByKeyName";
    desc.displayName = "Bind by key label";
    desc.description =
        "Store keybinds by the label on the key instead of its position. Leave off to keep WASD "
        "under your left hand on any keyboard layout.";
    desc.defaultValue = false;
    desc.visibility = config::Visibility::Advanced;
    return settings.registerBool(desc);
}

} // namespace vxe::input
