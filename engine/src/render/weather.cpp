#include "vxe/render/weather.hpp"

#include <vxe/math/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace vxe::render {
namespace {

constexpr std::array<Atmosphere, 4> kKinds = {{
    {.cloudCover = 0.06F, .fogScale = 0.4F, .clearness = 1.0F, .overcast = 0.0F},   // Clear.
    {.cloudCover = 0.34F, .fogScale = 0.8F, .clearness = 0.4F, .overcast = 0.0F},   // Fair.
    {.cloudCover = 0.64F, .fogScale = 1.25F, .clearness = 0.1F, .overcast = 0.25F}, // Cloudy.
    {.cloudCover = 0.95F, .fogScale = 2.2F, .clearness = 0.0F, .overcast = 0.8F},   // Overcast.
}};

// Out of 100, in the order of Weather: mostly fair, clear nights now and then,
// grey days rarely.
constexpr std::array<u32, 4> kShares = {22, 45, 24, 9};
constexpr u64 kWeatherSalt = 0x57EA7EE2ULL;

Atmosphere mixed(const Atmosphere& a, const Atmosphere& b, f32 t) noexcept {
    const auto lerp = [t](f32 from, f32 to) { return from + ((to - from) * t); };
    return {.cloudCover = lerp(a.cloudCover, b.cloudCover),
            .fogScale = lerp(a.fogScale, b.fogScale),
            .clearness = lerp(a.clearness, b.clearness),
            .overcast = lerp(a.overcast, b.overcast)};
}

// How the hour changes fog: a bell of mist around sunrise, thin air at midday,
// and a little more again through the cool of the night.
f32 fogOfHour(f64 hours) noexcept {
    const f64 fromMist = (hours - 6.5) / 1.7;
    const f64 mist = std::exp(-(fromMist * fromMist));
    const f64 fromNoon = (hours - 13.0) / 4.0;
    const f64 midday = std::exp(-(fromNoon * fromNoon));
    return static_cast<f32>(1.1 + (mist * 2.4) - (midday * 0.45));
}

} // namespace

Weather weatherOfSpell(u64 seed, i64 spell) noexcept {
    const auto roll = static_cast<u32>(noise::unit(noise::hash2(seed ^ kWeatherSalt, spell, 0)) * 100.0);
    u32 reached = 0;
    for (usize kind = 0; kind < kShares.size(); ++kind) {
        reached += kShares.at(kind);
        if (roll < reached) {
            return static_cast<Weather>(kind);
        }
    }
    return Weather::Fair;
}

Atmosphere computeAtmosphere(u64 seed, f64 clockHours, WeatherChoice choice) noexcept {
    Atmosphere out;
    if (choice != WeatherChoice::Auto) {
        out = kKinds.at(static_cast<usize>(choice) - 1);
    } else {
        const f64 spellAt = std::floor(clockHours / kWeatherSpellHours);
        const f64 inSpell = clockHours - (spellAt * kWeatherSpellHours);
        const auto spell = static_cast<i64>(spellAt);
        const f64 turning = std::clamp(
            (inSpell - (kWeatherSpellHours - kWeatherChangeHours)) / kWeatherChangeHours, 0.0, 1.0);
        const auto t = static_cast<f32>(turning * turning * (3.0 - (2.0 * turning)));
        out = mixed(kKinds.at(static_cast<usize>(weatherOfSpell(seed, spell))),
                    kKinds.at(static_cast<usize>(weatherOfSpell(seed, spell + 1))), t);
    }
    out.fogScale *= fogOfHour(std::fmod(std::max(clockHours, 0.0), 24.0));
    return out;
}

} // namespace vxe::render
