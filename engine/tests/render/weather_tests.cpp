#include <vxe/render/weather.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>

using namespace vxe;
using render::computeAtmosphere;
using render::Weather;
using render::WeatherChoice;

TEST_CASE("A world keeps its weather, and most of it is fair") {
    std::array<u32, 4> counts{};
    for (i64 spell = 0; spell < 4000; ++spell) {
        ++counts.at(static_cast<usize>(render::weatherOfSpell(42, spell)));
        CHECK(render::weatherOfSpell(42, spell) == render::weatherOfSpell(42, spell));
    }
    CHECK(counts[static_cast<usize>(Weather::Fair)] > counts[static_cast<usize>(Weather::Clear)]);
    CHECK(counts[static_cast<usize>(Weather::Clear)] > counts[static_cast<usize>(Weather::Overcast)]);
    CHECK(counts[static_cast<usize>(Weather::Overcast)] > 100);
}

TEST_CASE("Chosen weather holds, and each kind has its own air") {
    const auto at = [](WeatherChoice choice) { return computeAtmosphere(1, 37.0, choice); };
    const auto clear = at(WeatherChoice::Clear);
    const auto overcast = at(WeatherChoice::Overcast);
    CHECK(clear.cloudCover < 0.1F);
    CHECK(overcast.cloudCover > 0.9F);
    CHECK(clear.clearness == 1.0F);
    CHECK(overcast.clearness == 0.0F);
    CHECK(overcast.overcast > 0.5F);
    CHECK(clear.fogScale < overcast.fogScale);
    CHECK(computeAtmosphere(1, 37.0, WeatherChoice::Clear).cloudCover ==
          computeAtmosphere(99, 500.0, WeatherChoice::Clear).cloudCover);
}

TEST_CASE("Weather changes slowly") {
    f32 worst = 0.0F;
    render::Atmosphere last = computeAtmosphere(7, 0.0, WeatherChoice::Auto);
    for (f64 hours = 0.05; hours < 400.0; hours += 0.05) {
        const render::Atmosphere now = computeAtmosphere(7, hours, WeatherChoice::Auto);
        worst = std::max(worst, std::abs(now.cloudCover - last.cloudCover));
        last = now;
    }
    // 0.05 hours is 2.5 real seconds of a 20 minute day.
    CHECK(worst < 0.06F);
}

TEST_CASE("Fog follows the hour: mist after dawn, thin at midday") {
    const auto fog = [](f64 hours) { return computeAtmosphere(1, hours, WeatherChoice::Fair).fogScale; };
    CHECK(fog(6.5) > fog(13.0) * 2.0F);
    CHECK(fog(0.0) > fog(13.0));
}
