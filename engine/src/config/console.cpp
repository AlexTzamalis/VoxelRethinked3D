#include "vxe/config/console.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <system_error>

namespace vxe::config {
namespace {

char lowerAscii(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string toLower(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), lowerAscii);
    return lower;
}

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::ranges::equal(text.substr(0, prefix.size()), prefix,
                              [](char a, char b) { return lowerAscii(a) == lowerAscii(b); });
}

bool containsIgnoreCase(std::string_view text, std::string_view needle) {
    return toLower(text).find(toLower(needle)) != std::string::npos;
}

CommandOutput failure(std::string message) {
    return CommandOutput{.success = false, .lines = {std::move(message)}};
}

std::string describeSetting(const SettingsRegistry& settings, const SettingInfo& info) {
    const Result<SettingValue> current = settings.value(info.id);
    const std::string value = current ? displaySettingValue(info, current.value()) : "?";
    if (settings.isDefault(info.id)) {
        return std::format("{} = {}", info.id, value);
    }
    return std::format("{} = {} (default {})", info.id, value, displaySettingValue(info, info.defaultValue));
}

// Setting ids that start with `prefix`, for completion.
std::vector<std::string> completeSettingIds(const SettingsRegistry& settings, std::string_view prefix) {
    std::vector<std::string> ids;
    for (const SettingInfo* info : settings.all()) {
        if (startsWithIgnoreCase(info->id, prefix)) {
            ids.push_back(info->id);
        }
    }
    return ids;
}

} // namespace

Result<SettingValue> parseSettingValue(const SettingInfo& info, std::string_view text) {
    const auto invalid = [&](std::string_view expected) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("'{}' needs {}, not '{}'", info.id, expected, text));
    };
    switch (info.type) {
    case SettingType::Bool: {
        const std::string lower = toLower(text);
        if (lower == "true" || lower == "on" || lower == "1" || lower == "yes") {
            return SettingValue(true);
        }
        if (lower == "false" || lower == "off" || lower == "0" || lower == "no") {
            return SettingValue(false);
        }
        return invalid("on or off");
    }
    case SettingType::Int: {
        i32 value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return invalid("a whole number");
        }
        return SettingValue(value);
    }
    case SettingType::Float: {
        f32 value = 0.0F;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
            return invalid("a number");
        }
        return SettingValue(value);
    }
    case SettingType::Enum: {
        for (const EnumOption& option : info.options) {
            if (toLower(option.name) == toLower(text)) {
                return SettingValue(option.value);
            }
        }
        std::string names;
        for (const EnumOption& option : info.options) {
            names += names.empty() ? "" : ", ";
            names += option.name;
        }
        return invalid(std::format("one of {}", names));
    }
    case SettingType::String:
        return SettingValue(std::string(text));
    }
    return invalid("a value");
}

std::string displaySettingValue(const SettingInfo& info, const SettingValue& value) {
    switch (info.type) {
    case SettingType::Bool:
        return std::get<bool>(value) ? "on" : "off";
    case SettingType::Int:
        return std::to_string(std::get<i32>(value));
    case SettingType::Float:
        return std::format("{}", std::get<f32>(value));
    case SettingType::Enum: {
        const i32 raw = std::get<i32>(value);
        const auto option =
            std::ranges::find_if(info.options, [raw](const EnumOption& o) { return o.value == raw; });
        return option != info.options.end() ? option->name : std::to_string(raw);
    }
    case SettingType::String:
        return std::get<std::string>(value);
    }
    return {};
}

std::vector<std::string> tokenizeCommandLine(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;
    bool hasToken = false;
    for (usize i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                current += line[++i];
            } else if (c == '"') {
                inQuotes = false;
            } else {
                current += c;
            }
        } else if (c == '"') {
            inQuotes = true;
            hasToken = true;
        } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (hasToken) {
                tokens.push_back(std::move(current));
                current.clear();
                hasToken = false;
            }
        } else {
            current += c;
            hasToken = true;
        }
    }
    if (hasToken) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

void CommandRegistry::registerCommand(CommandDesc command) {
    std::string name = command.name;
    m_commands.insert_or_assign(std::move(name), std::move(command));
}

bool CommandRegistry::hasCommand(std::string_view name) const {
    return m_commands.contains(name);
}

CommandOutput CommandRegistry::execute(std::string_view line) {
    const std::vector<std::string> tokens = tokenizeCommandLine(line);
    if (tokens.empty()) {
        return {};
    }
    const auto found = m_commands.find(toLower(tokens.front()));
    if (found == m_commands.end()) {
        std::vector<std::string> similar;
        for (const auto& [name, command] : m_commands) {
            if (startsWithIgnoreCase(name, tokens.front().substr(0, 1))) {
                similar.push_back(name);
            }
        }
        std::ranges::sort(similar);
        std::string hint;
        for (const std::string& name : similar) {
            hint += hint.empty() ? " Did you mean: " : ", ";
            hint += name;
        }
        return failure(std::format("Unknown command '{}'.{}", tokens.front(), hint));
    }
    return found->second.handler(std::span(tokens).subspan(1));
}

std::vector<std::string> CommandRegistry::complete(std::string_view line) const {
    std::vector<std::string> tokens = tokenizeCommandLine(line);
    // A trailing space means the next word has started and is still empty.
    const bool startingNewWord = !line.empty() && std::isspace(static_cast<unsigned char>(line.back())) != 0;
    if (startingNewWord || tokens.empty()) {
        tokens.emplace_back();
    }

    std::vector<std::string> candidates;
    if (tokens.size() == 1) {
        for (const auto& [name, command] : m_commands) {
            if (startsWithIgnoreCase(name, tokens.front())) {
                candidates.push_back(name);
            }
        }
    } else {
        const auto found = m_commands.find(toLower(tokens.front()));
        if (found != m_commands.end() && found->second.completer) {
            const std::span<const std::string> arguments = std::span(tokens).subspan(1);
            const usize index = arguments.size() - 1;
            for (std::string& candidate : found->second.completer(arguments, index)) {
                if (startsWithIgnoreCase(candidate, arguments[index])) {
                    candidates.push_back(std::move(candidate));
                }
            }
        }
    }
    std::ranges::sort(candidates);
    return candidates;
}

std::vector<const CommandDesc*> CommandRegistry::commands() const {
    std::vector<const CommandDesc*> list;
    list.reserve(m_commands.size());
    for (const auto& [name, command] : m_commands) {
        list.push_back(&command);
    }
    std::ranges::sort(list, {}, &CommandDesc::name);
    return list;
}

namespace {

CommandOutput runSet(SettingsRegistry& settings, std::span<const std::string> arguments) {
    if (arguments.size() < 2) {
        return failure("Usage: set <setting> <value>");
    }
    const SettingInfo* info = settings.find(arguments[0]);
    if (info == nullptr) {
        return failure(std::format("No setting named '{}'. Try: find {}", arguments[0], arguments[0]));
    }
    // String settings take the rest of the line, so `set Name My World` works.
    std::string text = arguments[1];
    if (info->type == SettingType::String) {
        for (usize i = 2; i < arguments.size(); ++i) {
            text += ' ' + arguments[i];
        }
    }
    Result<SettingValue> parsed = parseSettingValue(*info, text);
    if (!parsed) {
        return failure(parsed.error().message());
    }
    if (Result<void> applied = settings.set(info->id, parsed.value()); !applied) {
        return failure(applied.error().message());
    }
    return CommandOutput{.success = true, .lines = {describeSetting(settings, *info)}};
}

std::vector<std::string> completeSet(const SettingsRegistry& settings, std::span<const std::string> arguments,
                                     usize index) {
    if (index == 0) {
        return completeSettingIds(settings, arguments[0]);
    }
    const SettingInfo* info = settings.find(arguments[0]);
    if (index != 1 || info == nullptr) {
        return {};
    }
    if (info->type == SettingType::Bool) {
        return {"on", "off"};
    }
    std::vector<std::string> names;
    names.reserve(info->options.size());
    for (const EnumOption& option : info->options) {
        names.push_back(option.name);
    }
    return names;
}

CommandOutput runGet(const SettingsRegistry& settings, std::span<const std::string> arguments) {
    if (arguments.empty()) {
        return failure("Usage: get <setting>");
    }
    const SettingInfo* info = settings.find(arguments[0]);
    if (info == nullptr) {
        return failure(std::format("No setting named '{}'", arguments[0]));
    }
    return CommandOutput{.success = true, .lines = {describeSetting(settings, *info)}};
}

CommandOutput runReset(SettingsRegistry& settings, std::span<const std::string> arguments) {
    if (arguments.empty()) {
        return failure("Usage: reset <setting> | reset <Category>.* | reset *");
    }
    const std::string& target = arguments[0];
    if (target == "*") {
        usize count = 0;
        for (const std::string& category : settings.categories()) {
            count += settings.resetCategory(category);
        }
        return CommandOutput{.success = true, .lines = {std::format("Reset {} settings", count)}};
    }
    if (target.ends_with(".*")) {
        const usize count = settings.resetCategory(std::string_view(target).substr(0, target.size() - 2));
        return CommandOutput{.success = true,
                             .lines = {std::format("Reset {} settings under {}", count, target)}};
    }
    if (Result<void> result = settings.reset(target); !result) {
        return failure(result.error().message());
    }
    return CommandOutput{.success = true, .lines = {describeSetting(settings, *settings.find(target))}};
}

CommandOutput runFind(const SettingsRegistry& settings, std::span<const std::string> arguments) {
    if (arguments.empty()) {
        return failure("Usage: find <text>");
    }
    CommandOutput output;
    for (const SettingInfo* info : settings.all()) {
        if (containsIgnoreCase(info->id, arguments[0]) ||
            containsIgnoreCase(info->displayName, arguments[0])) {
            output.lines.push_back(describeSetting(settings, *info));
        }
    }
    if (output.lines.empty()) {
        output.lines.push_back(std::format("No setting matches '{}'", arguments[0]));
    }
    return output;
}

bool isInCategory(std::string_view category, std::string_view prefix) {
    return prefix.empty() || category == prefix ||
           (category.starts_with(prefix) && category.size() > prefix.size() &&
            category[prefix.size()] == '.');
}

CommandOutput runList(const SettingsRegistry& settings, std::span<const std::string> arguments) {
    CommandOutput output;
    const std::string_view prefix = arguments.empty() ? std::string_view() : std::string_view(arguments[0]);
    for (const SettingInfo* info : settings.all()) {
        if (isInCategory(info->category, prefix)) {
            output.lines.push_back(describeSetting(settings, *info));
        }
    }
    return output;
}

CommandOutput runHelp(const CommandRegistry& commands, std::span<const std::string> arguments) {
    CommandOutput output;
    for (const CommandDesc* command : commands.commands()) {
        if (arguments.empty() || command->name == arguments[0]) {
            output.lines.push_back(std::format("{}  -  {}", command->usage, command->description));
        }
    }
    if (output.lines.empty()) {
        return failure(std::format("No command named '{}'", arguments[0]));
    }
    return output;
}

// "preset <group>" lists a group's presets and the current one; "preset
// <group> <name>" applies one (Section 16.2).
CommandOutput runPreset(SettingsRegistry& settings, std::span<const std::string> arguments) {
    if (arguments.empty() || arguments.size() > 2) {
        return failure("Usage: preset <group> [name]");
    }
    const std::vector<std::string> names = settings.presetNames(arguments[0]);
    if (names.empty()) {
        return failure(std::format("No presets in group '{}'", arguments[0]));
    }
    CommandOutput output;
    if (arguments.size() == 1) {
        std::string list;
        for (const std::string& name : names) {
            list += (list.empty() ? "" : ", ") + name;
        }
        output.lines.push_back(
            std::format("{}: {} (now {})", arguments[0], list, settings.currentPreset(arguments[0])));
        return output;
    }
    if (Result<void> applied = settings.applyPreset(arguments[0], arguments[1]); !applied) {
        return failure(applied.error().message());
    }
    output.lines.push_back(std::format("{} set to {}", arguments[0], arguments[1]));
    return output;
}

} // namespace

void registerSettingsCommands(CommandRegistry& commands, SettingsRegistry& settings) {
    const auto settingIdCompleter = [&settings](std::span<const std::string> arguments, usize index) {
        return index == 0 ? completeSettingIds(settings, arguments[0]) : std::vector<std::string>{};
    };
    commands.registerCommand(CommandDesc{
        .name = "set",
        .usage = "set <setting> <value>",
        .description = "Changes a setting. Takes effect as its apply mode says.",
        .handler =
            [&settings](std::span<const std::string> arguments) { return runSet(settings, arguments); },
        .completer = [&settings](std::span<const std::string> arguments,
                                 usize index) { return completeSet(settings, arguments, index); },
    });
    commands.registerCommand(CommandDesc{
        .name = "get",
        .usage = "get <setting>",
        .description = "Shows the current value of a setting.",
        .handler =
            [&settings](std::span<const std::string> arguments) { return runGet(settings, arguments); },
        .completer = settingIdCompleter,
    });
    commands.registerCommand(CommandDesc{
        .name = "reset",
        .usage = "reset <setting> | reset <Category>.* | reset *",
        .description = "Restores defaults for one setting, a whole category or everything.",
        .handler =
            [&settings](std::span<const std::string> arguments) { return runReset(settings, arguments); },
        .completer = settingIdCompleter,
    });
    commands.registerCommand(CommandDesc{
        .name = "find",
        .usage = "find <text>",
        .description = "Lists settings whose id or name contains the text.",
        .handler =
            [&settings](std::span<const std::string> arguments) { return runFind(settings, arguments); },
        .completer = nullptr,
    });
    commands.registerCommand(CommandDesc{
        .name = "list",
        .usage = "list [category]",
        .description = "Lists every setting, or the settings of one category.",
        .handler =
            [&settings](std::span<const std::string> arguments) { return runList(settings, arguments); },
        .completer =
            [&settings](std::span<const std::string> /*arguments*/, usize index) {
                return index == 0 ? settings.categories() : std::vector<std::string>{};
            },
    });
    commands.registerCommand(CommandDesc{
        .name = "preset",
        .usage = "preset <group> [name]",
        .description =
            "Lists a group's quality presets, or applies one (for example preset Graphics.LOD Ultra).",
        .handler =
            [&settings](std::span<const std::string> arguments) { return runPreset(settings, arguments); },
        .completer = nullptr,
    });
    commands.registerCommand(CommandDesc{
        .name = "help",
        .usage = "help [command]",
        .description = "Lists the commands, or explains one.",
        .handler =
            [&commands](std::span<const std::string> arguments) { return runHelp(commands, arguments); },
        .completer = nullptr,
    });
}

} // namespace vxe::config
