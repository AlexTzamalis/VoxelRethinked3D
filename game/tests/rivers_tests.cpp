#include "worldgen/planet.hpp"
#include "worldgen/rivers.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

using namespace vr;
using vxe::f64;
using vxe::i64;
using vxe::usize;

namespace {

// The stretches of a wide area, once built for all tests: a tile takes a while.
const std::vector<RiverNetwork::Stretch>& area() {
    static const Planet kPlanet(12345, PlanetPreset{});
    static const RiverNetwork kRivers(kPlanet);
    static const std::vector<RiverNetwork::Stretch> kArea = [] {
        std::vector<RiverNetwork::Stretch> out;
        kRivers.gather(-3000.0, -3000.0, 3000.0, 3000.0, out);
        return out;
    }();
    return kArea;
}

} // namespace

TEST_CASE("Water only runs downhill") {
    REQUIRE(area().size() > 200);
    for (const RiverNetwork::Stretch& stretch : area()) {
        CHECK(stretch.levelB <= stretch.levelA);
    }
}

TEST_CASE("Rivers grow as streams join them, and never shrink on the way") {
    usize grown = 0;
    f64 highUp = 0.0;
    f64 widest = 0.0;
    for (const RiverNetwork::Stretch& stretch : area()) {
        CHECK(stretch.widthB >= stretch.widthA);
        grown += stretch.widthB > stretch.widthA ? 1U : 0U;
        widest = std::max(widest, stretch.widthB);
        if (stretch.levelA > 120.0) {
            highUp = std::max(highUp, stretch.widthA);
        }
    }
    MESSAGE(area().size() << " stretches, widest " << widest << ", widest high up " << highUp);
    CHECK(grown > 50);
    CHECK(widest > 8.0);
    CHECK(highUp < widest * 0.6);
}

TEST_CASE("Streams join: many stretches can end where one goes on") {
    std::map<std::pair<f64, f64>, usize> arrivals;
    for (const RiverNetwork::Stretch& stretch : area()) {
        ++arrivals[{stretch.bx, stretch.bz}];
    }
    usize junctions = 0;
    for (const auto& [spot, count] : arrivals) {
        junctions += count > 1 ? 1U : 0U;
    }
    MESSAGE(junctions << " places where water joins");
    CHECK(junctions > 20);
}

TEST_CASE("A river only ever cuts the ground") {
    for (usize i = 0; i < area().size(); i += 37) {
        const RiverNetwork::Stretch& stretch = area()[i];
        for (const f64 height : {-5.0, 10.0, 80.0, 300.0}) {
            const auto carved = RiverNetwork::carve(area(), stretch.ax + 3.0, stretch.az, height);
            CHECK(carved.ground <= height);
        }
    }
}

TEST_CASE("The network is the same in any order, across tile borders") {
    const Planet planet(12345, PlanetPreset{});
    const RiverNetwork first(planet);
    const RiverNetwork second(planet);
    std::vector<RiverNetwork::Stretch> a;
    std::vector<RiverNetwork::Stretch> b;
    // Asked in another order, the second network builds its tiles differently.
    second.gather(9000.0, 9000.0, 9100.0, 9100.0, b);
    b.clear();
    first.gather(8100.0, 100.0, 8220.0, 220.0, a); // Across the border at 8160.
    second.gather(8100.0, 100.0, 8220.0, 220.0, b);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].ax == b[i].ax);
        CHECK(a[i].levelA == b[i].levelA);
        CHECK(a[i].widthB == b[i].widthB);
    }
}

TEST_CASE("Another world never sees the last world's rivers") {
    // On the stack, the second network gets the first one's address.
    const auto firstStretch = [](vxe::u64 seed) {
        const Planet planet(seed, PlanetPreset{});
        const RiverNetwork rivers(planet);
        std::vector<RiverNetwork::Stretch> found;
        rivers.gather(0.0, 0.0, 3000.0, 3000.0, found);
        REQUIRE(!found.empty());
        return std::pair(found.front().ax, found.size());
    };
    const auto first = firstStretch(1);
    const auto second = firstStretch(2);
    CHECK(first != second);
    CHECK(firstStretch(1) == first);
}
