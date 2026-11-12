#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace vr {

// The folder name of the game's player files inside the system's data folder
// (%APPDATA%\.voxelrethinked on Windows, ~/.voxelrethinked elsewhere).
inline constexpr std::string_view kUserFolderName = ".voxelrethinked";
// A file next to the program that keeps every player file beside it instead
// (shared copies, test copies made by build.cmd package).
inline constexpr std::string_view kPortableMarker = "portable.txt";

// Where the player's files go: next to the program when it runs portable
// (kPortableMarker beside it, or `portable` asked for), else the system's data
// folder plus kUserFolderName. Empty means next to the program as well, which
// is also the answer when the system names no data folder.
[[nodiscard]] std::filesystem::path chooseUserFolder(const std::filesystem::path& programFolder,
                                                     const std::filesystem::path& roamingFolder,
                                                     bool portable);

// The user folder holding a config folder, whether the path ends in a
// separator ("C:/x/.voxelrethinked/config/") or not.
[[nodiscard]] std::filesystem::path userFolderOf(const std::filesystem::path& configFolder);

// Copies the player's files from a program folder that kept them beside it
// (every build before the user folder) into a user folder that has none yet:
// config/ and saves/ as they are, and every texture pack in textures/ except
// the shipped ones (`shippedPacks`) into resourcepacks/. Nothing is moved or
// deleted, so the old copy stays. Does nothing when the user folder already
// has settings or worlds, or both folders are one. Returns the files copied.
[[nodiscard]] vxe::Result<vxe::usize> copyOldUserFiles(const std::filesystem::path& programFolder,
                                                       const std::filesystem::path& userFolder,
                                                       std::span<const std::string_view> shippedPacks);

} // namespace vr
