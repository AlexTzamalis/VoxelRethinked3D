#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vr {

// A new world starts in the morning, an hour after the sun came up.
inline constexpr vxe::f64 kFirstMorning = 7.0;

// What player.dat holds (Section 21).
struct PlayerData {
    vxe::DVec3 position{};
    vxe::f32 yaw = 0.0F;   // Radians.
    vxe::f32 pitch = 0.0F; // Radians.
    bool flying = false;
    vxe::u8 hotbarSlot = 0;
    std::array<std::string, 9> hotbar{}; // Block names, empty for free slots.
    vxe::f64 clockHours = kFirstMorning; // The world's clock (render::SkyRenderer).
};

// What world.toml says about a world (Section 21), plus its size on disk and
// its player, read along with it so entering a world needs no disk access.
// Until world generation exists (G3), the seed picks the demo terrain, and
// the type and preset are only stored.
struct WorldInfo {
    std::string folder;   // Inside saves/, also the world's stable id.
    std::string name;     // Shown to players; renaming keeps the folder.
    std::string seedText; // As typed. The seed, except for text seeds.
    vxe::u64 seed = 0;
    std::string worldType = "Default";
    std::string preset = "Default"; // The worldgen preset.
    vxe::i32 minY = -1024;          // Multiples of 16 (Section 9.2).
    vxe::i32 maxY = 3072;
    bool structures = true;
    // Far terrain baked before the world shows: blocks from the spawn, 0 for
    // none, -1 for the whole horizon (DemoWorldDesc::premapBlocks).
    vxe::i32 premapBlocks = -1;
    // The far terrain preset (Graphics.LOD) the world plays with: applied when
    // it is entered, taken from the settings when it is left. "Custom" leaves
    // the settings alone.
    std::string lodQuality = "High";
    vxe::i64 createdSeconds = 0; // Seconds since 1970.
    vxe::i64 lastPlayedSeconds = 0;
    std::string gameVersion;
    vxe::u64 sizeBytes = 0;           // Not stored; measured when scanning.
    std::optional<PlayerData> player; // Nothing in a world not played yet.
};

// The seed of what a player typed: whole numbers are used as they are, any
// other text is hashed, so "hello" is a seed like any number.
[[nodiscard]] vxe::u64 seedFromText(std::string_view text) noexcept;
// A random whole number for the seed field.
[[nodiscard]] std::string randomSeedText();

// An error when the heights are unusable: multiples of 16, with room between.
[[nodiscard]] vxe::Result<void> validateWorldHeight(vxe::i32 minY, vxe::i32 maxY);

// Every world in `saves`, last played first. Folders without a readable
// world.toml are skipped: they are not worlds, or are too broken to offer.
[[nodiscard]] std::vector<WorldInfo> scanWorlds(const std::filesystem::path& saves);

[[nodiscard]] vxe::Result<WorldInfo> loadWorldInfo(const std::filesystem::path& saves,
                                                   const std::string& folder);
// Writes world.toml atomically, so a crash leaves the old file or the new one.
[[nodiscard]] vxe::Result<void> saveWorldInfo(const std::filesystem::path& saves, const WorldInfo& info);

// Writes player.dat atomically.
[[nodiscard]] vxe::Result<void> savePlayerData(const std::filesystem::path& saves, const std::string& folder,
                                               const PlayerData& player);

// A folder name from a world's name that `saves` does not use yet: safe on
// every system, with a number added when the name is taken.
[[nodiscard]] std::string uniqueWorldFolder(const std::filesystem::path& saves, std::string_view name);

// Deletes a world's folder for good. Refuses anything but a direct child of
// `saves` that holds a world.toml, so a bad name can never reach other files.
[[nodiscard]] vxe::Result<void> deleteWorld(const std::filesystem::path& saves, const std::string& folder);

} // namespace vr
