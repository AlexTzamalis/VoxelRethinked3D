#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/result.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Console commands (Section 16.5 and E5.3).
//
// The registry turns a typed line such as `set Graphics.LOD.MaxDistance 16000`
// into a call, and offers completions while the line is being typed. The UI
// (an ImGui console in E7.4) only shows text; everything it needs, including
// autocomplete, comes from here, so the same commands can later be sent from
// a dedicated server prompt or a script.
namespace vxe::config {

// Parses text typed by a person into a value of the setting's type: "on",
// "true", "1" and "yes" for booleans, numbers for numeric settings, an option
// name (any case) for enums, and the text itself for strings.
[[nodiscard]] Result<SettingValue> parseSettingValue(const SettingInfo& info, std::string_view text);

// The value as a person reads it: enums by option name, floats without
// trailing zeros, strings as they are.
[[nodiscard]] std::string displaySettingValue(const SettingInfo& info, const SettingValue& value);

// What a command printed, and whether it worked.
struct CommandOutput {
    bool success = true;
    std::vector<std::string> lines;
};

// Splits a command line into words. Double quotes keep spaces inside one
// word ("My World"), and \" and \\ inside quotes are a quote and a backslash.
[[nodiscard]] std::vector<std::string> tokenizeCommandLine(std::string_view line);

using CommandHandler = std::function<CommandOutput(std::span<const std::string> arguments)>;
// Returns the candidates for the argument at `argumentIndex` (0 is the first
// argument after the command name), given everything typed so far.
using CommandCompleter =
    std::function<std::vector<std::string>(std::span<const std::string> arguments, usize argumentIndex)>;

struct CommandDesc {
    std::string name;        // One word, for example "set".
    std::string usage;       // For example "set <setting> <value>".
    std::string description; // One line for help.
    CommandHandler handler;
    CommandCompleter completer; // Optional.
};

// The table of console commands. Main thread only.
class CommandRegistry {
public:
    // Adds or replaces a command.
    void registerCommand(CommandDesc command);
    [[nodiscard]] bool hasCommand(std::string_view name) const;

    // Runs one line. Empty lines do nothing; unknown commands fail with a
    // hint to the closest command names.
    CommandOutput execute(std::string_view line);

    // Completions for the word at the end of `line` (the one being typed).
    // Each candidate is the complete replacement for that word, sorted.
    [[nodiscard]] std::vector<std::string> complete(std::string_view line) const;

    // Every command, sorted by name, for help.
    [[nodiscard]] std::vector<const CommandDesc*> commands() const;

private:
    StringMap<CommandDesc> m_commands;
};

// Adds set, get, reset, find, list and help, operating on `settings`. Both
// registries must outlive the commands.
void registerSettingsCommands(CommandRegistry& commands, SettingsRegistry& settings);

} // namespace vxe::config
