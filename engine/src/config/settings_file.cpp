#include "vxe/config/settings_file.hpp"

#include <vxe/io/file_io.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <sstream>

namespace vxe::config {
namespace {

std::string_view applyModeText(ApplyMode mode) noexcept {
    switch (mode) {
    case ApplyMode::Live:
        return "immediately";
    case ApplyMode::RequiresRemesh:
        return "after chunks are rebuilt (automatic)";
    case ApplyMode::RequiresRendererReload:
        return "after the renderer reloads (automatic)";
    case ApplyMode::RequiresWorldReload:
        return "after re-entering the world";
    case ApplyMode::RequiresRestart:
        return "after restarting the game";
    }
    return "immediately";
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// A TOML basic string: quotes, backslashes and control characters escaped.
std::string tomlString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04X}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

// Bare keys may only use letters, digits, "_" and "-"; anything else is quoted.
std::string tomlKey(std::string_view key) {
    const bool bare = !key.empty() && std::ranges::all_of(key, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
    });
    return bare ? std::string(key) : tomlString(key);
}

std::string tomlTableHeader(std::string_view category) {
    std::string header = "[";
    usize start = 0;
    while (start <= category.size()) {
        const usize dot = category.find('.', start);
        const std::string_view part =
            category.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (start != 0) {
            header += '.';
        }
        header += tomlKey(part);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return header + "]";
}

std::string formatValue(const SettingInfo& info, const SettingValue& value) {
    switch (info.type) {
    case SettingType::Bool:
        return std::get<bool>(value) ? "true" : "false";
    case SettingType::Int:
        return std::to_string(std::get<i32>(value));
    case SettingType::Float: {
        // Shortest text that reads back as the same float, and always with a
        // decimal point, so TOML keeps it a float ("1" would become an integer).
        std::string text = std::format("{}", std::get<f32>(value));
        if (text.find_first_of(".eEn") == std::string::npos) {
            text += ".0";
        }
        return text;
    }
    case SettingType::Enum: {
        const i32 raw = std::get<i32>(value);
        const auto option =
            std::ranges::find_if(info.options, [raw](const EnumOption& o) { return o.value == raw; });
        return option != info.options.end() ? tomlString(option->name) : std::to_string(raw);
    }
    case SettingType::String:
        return tomlString(std::get<std::string>(value));
    }
    return {};
}

std::string describeRange(const SettingInfo& info) {
    switch (info.type) {
    case SettingType::Int:
    case SettingType::Float:
        return std::format(", from {} to {}", info.min, info.max);
    case SettingType::Enum: {
        std::string names;
        for (const EnumOption& option : info.options) {
            names += names.empty() ? "" : ", ";
            names += option.name;
        }
        return std::format(", one of {}", names);
    }
    case SettingType::Bool:
    case SettingType::String:
        break;
    }
    return {};
}

// Turns a TOML value into the setting's type, or explains why it cannot.
Result<SettingValue> convert(const SettingInfo& info, const toml::node& node) {
    const auto mismatch = [&info](std::string_view expected) {
        return Error(ErrorCode::InvalidArgument, std::format("'{}' needs {}", info.id, expected));
    };
    switch (info.type) {
    case SettingType::Bool:
        if (const auto v = node.value<bool>(); v && node.is_boolean()) {
            return SettingValue(*v);
        }
        return mismatch("true or false");
    case SettingType::Int:
        if (node.is_integer()) {
            const i64 raw = node.as_integer()->get();
            const i64 clamped =
                std::clamp<i64>(raw, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max());
            return SettingValue(static_cast<i32>(clamped));
        }
        return mismatch("a whole number");
    case SettingType::Float:
        // A whole number is accepted too, because people write "fov = 90".
        if (node.is_floating_point()) {
            return SettingValue(static_cast<f32>(node.as_floating_point()->get()));
        }
        if (node.is_integer()) {
            return SettingValue(static_cast<f32>(node.as_integer()->get()));
        }
        return mismatch("a number");
    case SettingType::Enum:
        if (node.is_string()) {
            const std::string_view name = node.as_string()->get();
            for (const EnumOption& option : info.options) {
                if (equalsIgnoreCase(option.name, name)) {
                    return SettingValue(option.value);
                }
            }
            return Error(ErrorCode::InvalidArgument,
                         std::format("'{}' has no option '{}'{}", info.id, name, describeRange(info)));
        }
        if (node.is_integer()) {
            return SettingValue(static_cast<i32>(node.as_integer()->get()));
        }
        return mismatch("an option name");
    case SettingType::String:
        if (node.is_string()) {
            return SettingValue(std::string(node.as_string()->get()));
        }
        return mismatch("text in quotes");
    }
    return mismatch("a value");
}

std::string nodeToText(const toml::node& node) {
    std::ostringstream out;
    node.visit([&out](const auto& value) { out << toml::toml_formatter(value); });
    return out.str();
}

// Walks every value in the table, depth first, with its dotted path. The
// recursion is as deep as the table nesting of the file, a handful of levels.
// NOLINTNEXTLINE(misc-no-recursion)
void walk(const toml::table& table, const std::string& prefix, SettingsRegistry& registry, Scope scope,
          LoadReport& report) {
    for (const auto& [key, node] : table) {
        const std::string id =
            prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        if (const toml::table* nested = node.as_table()) {
            walk(*nested, id, registry, scope, report);
            continue;
        }
        const SettingInfo* info = registry.find(id);
        if (info == nullptr) {
            registry.setUnknown(scope, id, nodeToText(node));
            continue;
        }
        if (info->scope != scope) {
            report.warnings.push_back(std::format("'{}' belongs in the other settings file; ignored", id));
            continue;
        }
        Result<SettingValue> converted = convert(*info, node);
        if (!converted) {
            report.warnings.push_back(converted.error().message() + "; kept the current value");
            continue;
        }
        if (Result<void> applied = registry.set(id, converted.value()); !applied) {
            report.warnings.push_back(applied.error().message() + "; kept the current value");
            continue;
        }
        ++report.applied;
    }
}

} // namespace

Result<SettingValue> parseTomlSettingValue(const SettingInfo& info, std::string_view tomlValueText) {
    toml::table table;
    try {
        table = toml::parse(std::format("value = {}", tomlValueText));
    } catch (const toml::parse_error& error) {
        return Error(ErrorCode::ParseError, std::format("'{}' = {} is not valid TOML: {}", info.id,
                                                        tomlValueText, error.description()));
    }
    const toml::node* node = table.get("value");
    if (node == nullptr) {
        return Error(ErrorCode::ParseError, std::format("'{}' has no value", info.id));
    }
    return convert(info, *node);
}

Result<LoadReport> loadSettingsFromString(SettingsRegistry& registry, std::string_view toml, Scope scope) {
    toml::table root;
    // toml++ reports parse errors by exception. The engine does not throw
    // (Section 23.4), so the exception is turned into an error here.
    try {
        root = toml::parse(toml);
    } catch (const toml::parse_error& error) {
        const auto& where = error.source().begin;
        return Error(ErrorCode::ParseError,
                     std::format("Settings file is not valid TOML (line {}, column {}): {}", where.line,
                                 where.column, error.description()));
    }
    LoadReport report;
    walk(root, std::string(), registry, scope, report);
    return report;
}

Result<LoadReport> loadSettingsFromFile(SettingsRegistry& registry, const std::filesystem::path& path,
                                        Scope scope) {
    Result<std::vector<std::byte>> bytes = io::readFile(path);
    if (!bytes) {
        return std::move(bytes).error();
    }
    const std::vector<std::byte>& data = bytes.value();
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    return loadSettingsFromString(registry, text, scope);
}

std::string saveSettingsToString(const SettingsRegistry& registry, Scope scope) {
    // Category to its lines, sorted by category so the file is stable and
    // diffs stay readable (ADR-014).
    std::map<std::string, std::string> sections;
    for (const SettingInfo* info : registry.all()) {
        if (info->scope != scope) {
            continue;
        }
        const usize dot = info->id.rfind('.');
        const std::string key = dot == std::string::npos ? info->id : info->id.substr(dot + 1);
        std::string& section = sections[info->category];
        if (!section.empty()) {
            section += '\n';
        }
        const std::string_view name = info->displayName.empty() ? key : info->displayName;
        section += info->description.empty() ? std::format("# {}\n", name)
                                             : std::format("# {}: {}\n", name, info->description);
        section += std::format("# Default {}{}. Takes effect: {}.\n", formatValue(*info, info->defaultValue),
                               describeRange(*info), applyModeText(info->applyMode));
        const Result<SettingValue> current = registry.value(info->id);
        section += std::format("{} = {}\n", tomlKey(key), formatValue(*info, current.value()));
    }
    for (const auto& [id, text] : registry.unknown(scope)) {
        const usize dot = id.rfind('.');
        const std::string category = dot == std::string::npos ? std::string() : id.substr(0, dot);
        const std::string key = dot == std::string::npos ? id : id.substr(dot + 1);
        std::string& section = sections[category];
        if (!section.empty()) {
            section += '\n';
        }
        section += "# Not used by this version of the game; kept so other versions keep it.\n";
        section += std::format("{} = {}\n", tomlKey(key), text);
    }

    std::string out = scope == Scope::Global
                          ? "# Voxel Rethinked settings. Edit while the game is closed, or use the\n"
                            "# in game settings screen. Unknown keys are kept, not removed.\n"
                          : "# Settings of this world.\n";
    // Keys without a category must come before any table header.
    if (const auto top = sections.find(std::string()); top != sections.end()) {
        out += '\n' + top->second;
        sections.erase(top);
    }
    for (const auto& [category, lines] : sections) {
        out += '\n' + tomlTableHeader(category) + '\n' + lines;
    }
    return out;
}

Result<void> saveSettingsToFile(const SettingsRegistry& registry, const std::filesystem::path& path,
                                Scope scope) {
    const std::string text = saveSettingsToString(registry, scope);
    return io::writeFile(path, std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace vxe::config
