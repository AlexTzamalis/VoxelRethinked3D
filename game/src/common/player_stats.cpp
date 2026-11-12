#include "common/player_stats.hpp"

#include <vxe/io/file_io.hpp>

#include <toml++/toml.hpp>

#include <cmath>
#include <span>
#include <sstream>

namespace vr {
namespace {

// A teleport (the camera command, a respawn) is not travel.
constexpr vxe::f64 kMaxStepBlocks = 50.0;

vxe::u64 total(const std::map<std::string, vxe::u64, std::less<>>& counts) noexcept {
    vxe::u64 sum = 0;
    for (const auto& [name, count] : counts) {
        sum += count;
    }
    return sum;
}

} // namespace

vxe::u64 PlayerStats::placedTotal() const noexcept {
    return total(placed);
}

vxe::u64 PlayerStats::brokenTotal() const noexcept {
    return total(broken);
}

std::string_view blockOfState(std::string_view stateName) noexcept {
    return stateName.substr(0, stateName.find('['));
}

void StatsTracker::step(PlayerStats& stats, const PlayerState& now, bool inWater, bool sprinting,
                        vxe::f64 seconds) {
    stats.playSeconds += seconds;
    if (!m_started) {
        m_started = true;
        m_last = now;
        m_falling = false;
        return;
    }
    const vxe::DVec3 moved = now.position - m_last.position;
    const vxe::f64 across = std::hypot(moved.x, moved.z);
    if (std::abs(moved.y) + across > kMaxStepBlocks) {
        m_last = now;
        m_falling = false;
        return;
    }
    if (now.flying) {
        stats.flown += std::hypot(across, moved.y);
    } else if (inWater) {
        stats.swum += std::hypot(across, moved.y);
    } else {
        if (now.onGround) {
            if (now.crouching) {
                stats.crouched += across;
            } else if (sprinting) {
                stats.sprinted += across;
            } else {
                stats.walked += across;
            }
        }
        // Down through the air, the landing frame included; not walking downhill.
        if (moved.y < 0.0 && !(m_last.onGround && now.onGround)) {
            stats.fallen += -moved.y;
        }
    }
    // A jump leaves the ground going up on its own; a fall is measured from
    // where the ground was left to where it is found again.
    const bool airborne = !now.onGround && !now.flying && !inWater;
    if (m_last.onGround && airborne && now.velocity.y > 0.0) {
        ++stats.jumps;
    }
    if (airborne && !m_falling) {
        m_falling = true;
        m_fallStart = m_last.position.y;
    } else if (!airborne && m_falling) {
        m_falling = false;
        if (now.onGround && m_fallStart - now.position.y > PlayerStats::kLongFall) {
            ++stats.falls;
        }
    }
    if (m_falling) {
        m_fallStart = std::max(m_fallStart, now.position.y); // From the top of a jump.
    }
    m_last = now;
}

PlayerStats loadPlayerStats(const std::filesystem::path& file) {
    PlayerStats stats;
    const auto bytes = vxe::io::readFile(file);
    if (!bytes) {
        return stats; // A world not played yet, or from before statistics.
    }
    toml::table table;
    try {
        table = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.value().data()), // NOLINT
                                             bytes.value().size()));
    } catch (const toml::parse_error&) {
        return stats; // Counting starts over rather than refusing the world.
    }
    stats.playSeconds = table["play_seconds"].value_or(0.0);
    stats.walked = table["walked"].value_or(0.0);
    stats.sprinted = table["sprinted"].value_or(0.0);
    stats.crouched = table["crouched"].value_or(0.0);
    stats.swum = table["swum"].value_or(0.0);
    stats.flown = table["flown"].value_or(0.0);
    stats.fallen = table["fallen"].value_or(0.0);
    stats.jumps = static_cast<vxe::u64>(table["jumps"].value_or(vxe::i64{0}));
    stats.falls = static_cast<vxe::u64>(table["falls"].value_or(vxe::i64{0}));
    const auto counts = [&table](std::string_view key, std::map<std::string, vxe::u64, std::less<>>& out) {
        if (const toml::table* section = table[key].as_table(); section != nullptr) {
            for (const auto& [name, value] : *section) {
                if (const auto count = value.value<vxe::i64>(); count && *count > 0) {
                    out[std::string(name.str())] = static_cast<vxe::u64>(*count);
                }
            }
        }
    };
    counts("placed", stats.placed);
    counts("broken", stats.broken);
    return stats;
}

vxe::Result<void> savePlayerStats(const std::filesystem::path& file, const PlayerStats& stats) {
    toml::table table;
    table.insert("play_seconds", stats.playSeconds);
    table.insert("walked", stats.walked);
    table.insert("sprinted", stats.sprinted);
    table.insert("crouched", stats.crouched);
    table.insert("swum", stats.swum);
    table.insert("flown", stats.flown);
    table.insert("fallen", stats.fallen);
    table.insert("jumps", static_cast<vxe::i64>(stats.jumps));
    table.insert("falls", static_cast<vxe::i64>(stats.falls));
    const auto counts = [&table](std::string_view key,
                                 const std::map<std::string, vxe::u64, std::less<>>& in) {
        toml::table section;
        for (const auto& [name, count] : in) {
            section.insert(name, static_cast<vxe::i64>(count));
        }
        table.insert(key, std::move(section));
    };
    counts("placed", stats.placed);
    counts("broken", stats.broken);
    std::ostringstream text;
    text << table << '\n';
    const std::string out = text.str();
    return vxe::io::writeFile(file, std::as_bytes(std::span(out)));
}

} // namespace vr
