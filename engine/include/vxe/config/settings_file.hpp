#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Reading and writing settings as TOML (ADR-014, Section 16).
//
// The file is meant to be read and edited by people, so it is written with a
// comment above every setting (its name, description, default and range) and
// grouped into one table per category:
//
//   [Graphics.LOD]
//   # Max distance: How far LOD terrain reaches, in blocks.
//   # Default 16000, from 1000 to 64000. Takes effect: immediately.
//   MaxDistance = 16000
//
// Loading is forgiving: a bad value, a wrong type or a typo never stops the
// game. Each problem becomes a warning, the setting keeps its current value,
// and keys no setting knows are kept and written back on the next save.
namespace vxe::config {

struct LoadReport {
    usize applied = 0;                 // Values that were set.
    std::vector<std::string> warnings; // One line per problem, with the key.
};

// Parses TOML text and applies every value of `scope` to the registry.
// Fails only when the text is not valid TOML at all.
[[nodiscard]] Result<LoadReport> loadSettingsFromString(SettingsRegistry& registry, std::string_view toml,
                                                        Scope scope = Scope::Global);

// Reads a file and applies it. NotFound when the file does not exist yet,
// which on first start is normal: the caller then saves the defaults.
[[nodiscard]] Result<LoadReport> loadSettingsFromFile(SettingsRegistry& registry,
                                                      const std::filesystem::path& path,
                                                      Scope scope = Scope::Global);

// The TOML text for every setting of `scope`, plus the kept unknown keys.
[[nodiscard]] std::string saveSettingsToString(const SettingsRegistry& registry, Scope scope = Scope::Global);

// Writes saveSettingsToString atomically (temp file, then rename).
[[nodiscard]] Result<void> saveSettingsToFile(const SettingsRegistry& registry,
                                              const std::filesystem::path& path, Scope scope = Scope::Global);

} // namespace vxe::config
