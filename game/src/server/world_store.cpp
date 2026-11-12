#include "world_store.hpp"

#include <vxe/core/error.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/io/binary.hpp>
#include <vxe/io/file_io.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <format>
#include <random>
#include <span>
#include <sstream>
#include <system_error>

namespace vr {
namespace {

namespace fs = std::filesystem;
using vxe::Error;
using vxe::ErrorCode;
using vxe::i32;
using vxe::i64;
using vxe::Result;
using vxe::u64;

constexpr std::string_view kInfoFile = "world.toml";
constexpr std::string_view kPlayerFile = "player.dat";
constexpr vxe::u8 kPlayerVersion = 2; // 2 added the clock.
constexpr vxe::usize kMaxFolderLength = 32;

std::string asText(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; // NOLINT(*-reinterpret-cast)
}

// A folder must be one plain name, so joining it to saves/ stays inside it.
bool isPlainFolder(const std::string& folder) {
    const fs::path path(folder);
    return !folder.empty() && folder != "." && folder != ".." && !path.has_parent_path() &&
           !path.has_root_path();
}

// Nothing for a missing or damaged file: the player then starts at the spawn,
// which is better than refusing a world over its player file.
std::optional<PlayerData> loadPlayerData(const fs::path& file) {
    const auto bytes = vxe::io::readFile(file);
    if (!bytes) {
        return std::nullopt;
    }
    vxe::io::BinaryReader reader(bytes.value());
    const vxe::u8 version = reader.readU8();
    if (version == 0 || version > kPlayerVersion) {
        return std::nullopt;
    }
    PlayerData player;
    player.position.x = reader.readF64();
    player.position.y = reader.readF64();
    player.position.z = reader.readF64();
    player.yaw = reader.readF32();
    player.pitch = reader.readF32();
    player.flying = reader.readBool();
    player.hotbarSlot = reader.readU8();
    for (std::string& block : player.hotbar) {
        block = reader.readString();
    }
    if (version >= 2) {
        player.clockHours = reader.readF64();
    }
    if (!reader.ok() || player.hotbarSlot >= player.hotbar.size()) {
        return std::nullopt;
    }
    return player;
}

// Everything under a folder. Files that vanish meanwhile just do not count.
u64 folderSize(const fs::path& folder) {
    u64 total = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (it->is_regular_file(entryError) && !entryError) {
            const auto size = it->file_size(entryError);
            total += entryError ? 0U : static_cast<u64>(size);
        }
    }
    return total;
}

} // namespace

u64 seedFromText(std::string_view text) noexcept {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    u64 number = 0;
    if (const auto [next, error] = std::from_chars(begin, end, number); error == std::errc() && next == end) {
        return number;
    }
    i64 negative = 0;
    if (const auto [next, error] = std::from_chars(begin, end, negative);
        error == std::errc() && next == end) {
        return static_cast<u64>(negative);
    }
    return vxe::hashString(text);
}

std::string randomSeedText() {
    std::random_device device;
    const u64 seed = (static_cast<u64>(device()) << 32U) | device();
    return std::to_string(seed);
}

Result<void> validateWorldHeight(i32 minY, i32 maxY) {
    if (minY % 16 != 0 || maxY % 16 != 0) {
        return Error(ErrorCode::InvalidArgument, "World heights must be multiples of 16.");
    }
    if (maxY - minY < 16) {
        return Error(ErrorCode::InvalidArgument, "Max Y must be above Min Y.");
    }
    return {};
}

Result<WorldInfo> loadWorldInfo(const fs::path& saves, const std::string& folder) {
    if (!isPlainFolder(folder)) {
        return Error(ErrorCode::InvalidArgument, std::format("'{}' is not a world folder", folder));
    }
    auto bytes = vxe::io::readFile(saves / folder / kInfoFile);
    if (!bytes) {
        return bytes.error();
    }
    toml::table table;
    try {
        table = toml::parse(asText(bytes.value()));
    } catch (const toml::parse_error& error) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("{}/world.toml: {}", folder, error.description()));
    }
    WorldInfo info;
    info.folder = folder;
    const std::optional<std::string> name = table["name"].value<std::string>();
    if (!name || name->empty()) {
        return Error(ErrorCode::InvalidArgument, std::format("{}/world.toml has no name", folder));
    }
    info.name = *name;
    info.seedText = table["seed_text"].value_or(std::string());
    info.seed = seedFromText(table["seed"].value_or(std::string()));
    info.worldType = table["world_type"].value_or(info.worldType);
    info.preset = table["preset"].value_or(info.preset);
    info.minY = table["min_y"].value_or(info.minY);
    info.maxY = table["max_y"].value_or(info.maxY);
    info.structures = table["structures"].value_or(info.structures);
    // "premap_blocks" of worlds made before the option did anything is left
    // unread: those worlds keep the whole horizon they always had.
    info.premapBlocks = table["premap_distance"].value_or(info.premapBlocks);
    info.lodQuality = table["lod_quality"].value_or(info.lodQuality);
    info.createdSeconds = table["created"].value_or(i64{0});
    info.lastPlayedSeconds = table["last_played"].value_or(i64{0});
    info.gameVersion = table["game_version"].value_or(std::string());
    info.player = loadPlayerData(saves / folder / kPlayerFile);
    return info;
}

Result<void> savePlayerData(const fs::path& saves, const std::string& folder, const PlayerData& player) {
    if (!isPlainFolder(folder)) {
        return Error(ErrorCode::InvalidArgument, std::format("'{}' is not a world folder", folder));
    }
    vxe::io::BinaryWriter writer;
    writer.writeU8(kPlayerVersion);
    writer.writeF64(player.position.x);
    writer.writeF64(player.position.y);
    writer.writeF64(player.position.z);
    writer.writeF32(player.yaw);
    writer.writeF32(player.pitch);
    writer.writeBool(player.flying);
    writer.writeU8(player.hotbarSlot);
    for (const std::string& block : player.hotbar) {
        writer.writeString(block);
    }
    writer.writeF64(player.clockHours);
    return vxe::io::writeFile(saves / folder / kPlayerFile, writer.bytes());
}

Result<void> saveWorldInfo(const fs::path& saves, const WorldInfo& info) {
    if (!isPlainFolder(info.folder)) {
        return Error(ErrorCode::InvalidArgument, std::format("'{}' is not a world folder", info.folder));
    }
    // The seed is a string because TOML integers are signed.
    toml::table table;
    table.insert("name", info.name);
    table.insert("seed", std::to_string(info.seed));
    table.insert("seed_text", info.seedText);
    table.insert("world_type", info.worldType);
    table.insert("preset", info.preset);
    table.insert("min_y", info.minY);
    table.insert("max_y", info.maxY);
    table.insert("structures", info.structures);
    table.insert("premap_distance", info.premapBlocks);
    table.insert("lod_quality", info.lodQuality);
    table.insert("created", info.createdSeconds);
    table.insert("last_played", info.lastPlayedSeconds);
    table.insert("game_version", info.gameVersion);
    std::ostringstream text;
    text << table << '\n';
    const std::string out = text.str();
    return vxe::io::writeFile(saves / info.folder / kInfoFile, std::as_bytes(std::span(out)));
}

std::vector<WorldInfo> scanWorlds(const fs::path& saves) {
    std::vector<WorldInfo> worlds;
    std::error_code error;
    for (fs::directory_iterator it(saves, error), end; !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (!it->is_directory(entryError) || entryError) {
            continue;
        }
        auto info = loadWorldInfo(saves, it->path().filename().string());
        if (info) {
            info.value().sizeBytes = folderSize(it->path());
            worlds.push_back(std::move(info.value()));
        }
    }
    std::ranges::sort(worlds, [](const WorldInfo& a, const WorldInfo& b) {
        return a.lastPlayedSeconds != b.lastPlayedSeconds ? a.lastPlayedSeconds > b.lastPlayedSeconds
                                                          : a.name < b.name;
    });
    return worlds;
}

std::string uniqueWorldFolder(const fs::path& saves, std::string_view name) {
    std::string base;
    for (const char c : name) {
        const auto letter = static_cast<unsigned char>(c);
        if (std::isalnum(letter) != 0 || c == '-' || c == '_') {
            base.push_back(c);
        } else if (c == ' ' && !base.empty() && base.back() != '_') {
            base.push_back('_');
        }
    }
    base.resize(std::min(base.size(), kMaxFolderLength));
    while (!base.empty() && base.back() == '_') {
        base.pop_back();
    }
    // Windows refuses these names with any extension, and "demo" is the demo
    // world's far terrain cache.
    static constexpr std::array<std::string_view, 7> kReserved = {"con",  "prn",  "aux", "nul",
                                                                  "com1", "lpt1", "demo"};
    std::string lowered = base;
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (base.empty() || std::ranges::find(kReserved, lowered) != kReserved.end()) {
        base = "world_" + base;
    }
    std::string folder = base;
    std::error_code error;
    for (int number = 2; fs::exists(saves / folder, error); ++number) {
        folder = std::format("{}_{}", base, number);
    }
    return folder;
}

Result<void> deleteWorld(const fs::path& saves, const std::string& folder) {
    if (!isPlainFolder(folder)) {
        return Error(ErrorCode::InvalidArgument, std::format("'{}' is not a world folder", folder));
    }
    const fs::path path = saves / folder;
    std::error_code error;
    if (!fs::exists(path / kInfoFile, error)) {
        return Error(ErrorCode::NotFound, std::format("'{}' is not a world", folder));
    }
    fs::remove_all(path, error);
    if (error) {
        return Error(ErrorCode::IoError, std::format("Could not delete '{}': {}", folder, error.message()));
    }
    return {};
}

} // namespace vr
