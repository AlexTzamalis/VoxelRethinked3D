#pragma once

#include <vxe/core/types.hpp>

namespace vxe::render {

// The kinds of weather, in the order of Graphics.Sky.Weather after Auto.
enum class Weather : u8 {
    Clear,
    Fair,
    Cloudy,
    Overcast,
    Rain
};

// Graphics.Sky.Weather: follow the world's own weather, or hold one kind.
enum class WeatherChoice : u8 {
    Auto,
    Clear,
    Fair,
    Cloudy,
    Overcast,
    Rain
};

inline constexpr f64 kWeatherSpellHours = 6.0;  // Game hours one kind of weather lasts.
inline constexpr f64 kWeatherChangeHours = 2.0; // Game hours it takes to turn into the next.

// What the air is like at one moment. Everything that follows the weather and
// the time reads this one state: sky, fog, clouds, light and the stars, and
// later biomes and shader packs.
struct Atmosphere {
    f32 cloudCover = 0.3F;    // Share of the sky under clouds, 0 to 1.
    f32 fogScale = 1.0F;      // Times the fog density setting.
    f32 clearness = 0.3F;     // 0 to 1: how far the eye reaches at night. 1 shows the galaxy.
    f32 overcast = 0.0F;      // 0 to 1: how grey and dim the day is.
    f32 precipitation = 0.0F; // 0 to 1: how hard it rains, or snows where it is cold.
};

// The kind of weather of a spell, a pure function of the seed: a world keeps
// its weather, and every player of it would see the same.
[[nodiscard]] Weather weatherOfSpell(u64 seed, i64 spell) noexcept;

// The atmosphere at a time on the world's clock (hours since its first
// midnight). Spells turn into each other over kWeatherChangeHours, so clouds
// grow and thin instead of switching. Fog also follows the hour: mist after
// dawn, sharper air at midday.
[[nodiscard]] Atmosphere computeAtmosphere(u64 seed, f64 clockHours, WeatherChoice choice) noexcept;

} // namespace vxe::render
