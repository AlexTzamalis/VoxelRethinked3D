#pragma once

#include "common/player_physics.hpp"

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace vr {

// What a player did in one world (asked by Alex 2026-11-12, like Minecraft's
// statistics): time, distances by way of moving, jumps and long falls, and
// blocks placed and broken by block. Saved as stats.toml in the world folder,
// readable by hand.
struct PlayerStats {
    vxe::f64 playSeconds = 0.0; // In the world, not paused.
    // Blocks travelled.
    vxe::f64 walked = 0.0;
    vxe::f64 sprinted = 0.0;
    vxe::f64 crouched = 0.0;
    vxe::f64 swum = 0.0;
    vxe::f64 flown = 0.0;
    vxe::f64 fallen = 0.0; // Downwards through the air, not flying.
    vxe::u64 jumps = 0;
    vxe::u64 falls = 0; // Landings after a drop of more than kLongFall blocks.
    std::map<std::string, vxe::u64, std::less<>> placed; // By block name ("oak_log").
    std::map<std::string, vxe::u64, std::less<>> broken;

    // A fall counts once the drop is longer than this, as one that would hurt.
    static constexpr vxe::f64 kLongFall = 3.0;

    [[nodiscard]] vxe::u64 placedTotal() const noexcept;
    [[nodiscard]] vxe::u64 brokenTotal() const noexcept;
};

// The block of a state name: "oak_log[axis=y]" is "oak_log".
[[nodiscard]] std::string_view blockOfState(std::string_view stateName) noexcept;

// Counts movement into PlayerStats from the player's state between frames.
//
// Thread safety: main thread only, like the player.
class StatsTracker {
public:
    // One frame of play: the state before and after, whether the player is
    // in water and sprinting. Adds the frame's time and distance to `stats`.
    void step(PlayerStats& stats, const PlayerState& now, bool inWater, bool sprinting, vxe::f64 seconds);
    // Forgets the last state, so a teleport or a new world adds no distance.
    void reset() noexcept { m_started = false; }

private:
    PlayerState m_last{};
    bool m_started = false;
    bool m_falling = false;
    vxe::f64 m_fallStart = 0.0; // Height where the player left the ground.
};

[[nodiscard]] PlayerStats loadPlayerStats(const std::filesystem::path& file);
[[nodiscard]] vxe::Result<void> savePlayerStats(const std::filesystem::path& file, const PlayerStats& stats);

} // namespace vr
