#include "common/player_stats.hpp"
#include "ui/screens/statistics_screen.hpp"

#include <doctest/doctest.h>

#include <filesystem>

using namespace vr;

namespace {

PlayerState at(vxe::f64 x, vxe::f64 y, bool ground, vxe::f64 rising = 0.0) {
    PlayerState state;
    state.position = vxe::DVec3(x, y, 0.0);
    state.velocity = vxe::DVec3(0.0, rising, 0.0);
    state.onGround = ground;
    return state;
}

} // namespace

TEST_CASE("Statistics count distance by way of moving, jumps and long falls") {
    PlayerStats stats;
    StatsTracker tracker;
    tracker.step(stats, at(0.0, 64.0, true), false, false, 0.05); // Starts.
    tracker.step(stats, at(2.0, 64.0, true), false, false, 0.05); // Walks 2.
    tracker.step(stats, at(5.0, 64.0, true), false, true, 0.05);  // Sprints 3.
    CHECK(stats.walked == doctest::Approx(2.0));
    CHECK(stats.sprinted == doctest::Approx(3.0));
    // A jump on flat ground: up, then back down. Not a long fall.
    tracker.step(stats, at(5.0, 64.4, false, 4.0), false, false, 0.05);
    tracker.step(stats, at(5.0, 65.2, false, 1.0), false, false, 0.05);
    tracker.step(stats, at(5.0, 64.0, true), false, false, 0.05);
    CHECK(stats.jumps == 1);
    CHECK(stats.falls == 0);
    CHECK(stats.fallen == doctest::Approx(1.2));
    // Walking off a ledge 6 blocks high is a long fall, not a jump.
    tracker.step(stats, at(6.0, 63.9, false, -0.5), false, false, 0.05);
    tracker.step(stats, at(6.0, 58.0, true), false, false, 0.05);
    CHECK(stats.jumps == 1);
    CHECK(stats.falls == 1);
    // Swimming and flying.
    tracker.step(stats, at(9.0, 58.0, false), true, false, 0.05);
    PlayerState flying = at(13.0, 61.0, false);
    flying.flying = true;
    tracker.step(stats, flying, false, false, 0.05);
    CHECK(stats.swum == doctest::Approx(3.0));
    CHECK(stats.flown == doctest::Approx(5.0));
    // A teleport adds nothing.
    tracker.step(stats, at(5000.0, 70.0, true), false, false, 0.05);
    CHECK(stats.walked == doctest::Approx(2.0));
    CHECK(stats.playSeconds == doctest::Approx(0.05 * 11));
}

TEST_CASE("Statistics round trip through stats.toml") {
    const auto file = std::filesystem::temp_directory_path() / "vr_stats_tests" / "stats.toml";
    std::filesystem::remove(file);
    CHECK(loadPlayerStats(file).placedTotal() == 0); // No file: nothing counted yet.
    PlayerStats stats;
    stats.playSeconds = 3725.5;
    stats.walked = 1234.5;
    stats.jumps = 17;
    stats.placed["oak_log"] = 12;
    stats.placed["stone"] = 3;
    stats.broken["grass_block"] = 40;
    REQUIRE(savePlayerStats(file, stats).hasValue());
    const PlayerStats read = loadPlayerStats(file);
    CHECK(read.playSeconds == doctest::Approx(3725.5));
    CHECK(read.walked == doctest::Approx(1234.5));
    CHECK(read.jumps == 17);
    CHECK(read.placedTotal() == 15);
    CHECK(read.broken.at("grass_block") == 40);
    CHECK(blockOfState("oak_log[axis=y]") == "oak_log");
    CHECK(blockOfState("stone") == "stone");
}

TEST_CASE("Statistics read as short words and numbers") {
    CHECK(playTimeText(40.0) == "40 s");
    CHECK(playTimeText(12.0 * 60.0) == "12 min");
    CHECK(playTimeText(3725.0) == "1 h 02 min");
    CHECK(distanceText(850.4) == "850 m");
    CHECK(distanceText(12400.0) == "12.4 km");
}
