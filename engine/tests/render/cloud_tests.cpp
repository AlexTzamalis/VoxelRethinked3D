#include <vxe/render/clouds.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>

using namespace vxe;

TEST_CASE("A cloud map belongs to its seed, and its threshold is the share of sky") {
    const auto map = render::buildCloudMap(77);
    CHECK(map == render::buildCloudMap(77));
    CHECK(map != render::buildCloudMap(78));
    constexpr usize kSize = render::kCloudMapSize;
    REQUIRE(map[0].size() == kSize * kSize);
    const auto above = [&](u8 threshold) {
        const auto count = std::ranges::count_if(map[0], [&](u8 value) { return value > threshold; });
        return static_cast<f64>(count) / static_cast<f64>(kSize * kSize);
    };
    CHECK(above(153) == doctest::Approx(0.4).epsilon(0.02));
    CHECK(above(204) == doctest::Approx(0.2).epsilon(0.02));
}

TEST_CASE("A cloud map repeats without a seam") {
    const auto map = render::buildCloudMap(5);
    constexpr usize kSize = render::kCloudMapSize;
    // Across the wrap, neighbors differ no more than neighbors inside the map.
    i32 worstInside = 0;
    i32 worstAcross = 0;
    for (usize y = 0; y < kSize; ++y) {
        const auto at = [&](usize x) { return i32{map[0][(y * kSize) + x]}; };
        for (usize x = 0; x + 1 < kSize; ++x) {
            worstInside = std::max(worstInside, std::abs(at(x) - at(x + 1)));
        }
        worstAcross = std::max(worstAcross, std::abs(at(kSize - 1) - at(0)));
    }
    CHECK(worstAcross <= worstInside);
}

TEST_CASE("Each cloud map level is half the size of the last") {
    const auto map = render::buildCloudMap(9);
    for (usize level = 0; level < render::kCloudMapLevels; ++level) {
        const usize size = usize{render::kCloudMapSize} >> level;
        CHECK(map.at(level).size() == size * size);
    }
}
