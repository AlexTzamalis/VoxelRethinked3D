#pragma once

#include "planet.hpp"

#include <vxe/core/types.hpp>

#include <array>
#include <string_view>

namespace vr {

// Every biome of the world. What a biome looks like comes with G3.4 and G3.7.
enum class Biome : vxe::u8 {
    DeepOcean,
    Ocean,
    WarmOcean,
    FrozenOcean,
    Beach,
    SnowyBeach,
    StonyShore,
    Swamp,
    SnowyPeaks,
    RockyPeaks,
    WoodedSlopes,
    Meadow,
    SnowyPlains,
    SnowyTaiga,
    Taiga,
    Shrubland,
    Plains,
    FlowerPlains,
    Forest,
    BirchForest,
    FlowerForest,
    DarkForest,
    PaleForest,
    Savanna,
    Desert,
    Badlands,
    Jungle,
};
inline constexpr vxe::usize kBiomeCount = 27;

// Biome names for tools and logs.
inline constexpr std::array<std::string_view, kBiomeCount> kBiomeNames = {
    "Deep Ocean",  "Ocean",       "Warm Ocean",  "Frozen Ocean",  "Beach",    "Snowy Beach",  "Stony Shore",
    "Swamp",       "Snowy Peaks", "Rocky Peaks", "Wooded Slopes", "Meadow",   "Snowy Plains", "Snowy Taiga",
    "Taiga",       "Shrubland",   "Plains",      "Flower Plains", "Forest",   "Birch Forest", "Flower Forest",
    "Dark Forest", "Pale Forest", "Savanna",     "Desert",        "Badlands", "Jungle",
};

// The weather of one position.
struct ClimateSample {
    vxe::f64 temperature = 0.0; // Degrees Celsius at this height.
    vxe::f64 rainfall = 0.0;    // 0 is bone dry, 1 is the wettest.
    vxe::f64 shadow = 0.0;      // 0 to 1: how much mountains upwind take away.
    vxe::f64 variety = 0.0;     // 0 to 1: picks among biomes of the same climate.
};

// The climate layer (G3.2): temperature and rain, and the biome they make.
//
// Temperature falls with height, so peaks turn cold by themselves. Rain comes
// in on the wind: it falls on the side of a range that faces the wind and is
// gone on the far side, which makes real rain shadows, such as a desert behind
// mountains. Biomes only read these fields and the planet's, and never shape
// the ground, so a border between biomes can never make a cliff.
//
// Temperature at sea level and rain change slowly, so they are computed on a
// grid of kGridSize blocks and blended between, which keeps looking upwind
// cheap. Grid points are a pure function of the seed, so the result does not
// depend on what was sampled before.
//
// Thread safety: read only after construction. Keeps a reference to the
// planet, which must outlive it.
class Climate {
public:
    static constexpr vxe::i64 kGridSize = 32;

    Climate(vxe::u64 seed, const Planet& planet) noexcept;

    // The slow fields at one grid point.
    struct Field {
        vxe::f64 temperature = 0.0; // At sea level.
        vxe::f64 rainfall = 0.0;
        vxe::f64 shadow = 0.0;
        vxe::f64 variety = 0.0;
    };

    // The four corners of a grid cell. A chunk column lies inside one cell
    // (32 is a multiple of the chunk size, and both start at 0), so generating
    // it computes a cell once, the costly part, and then samples it for free.
    struct Cell {
        vxe::f64 coolPerBlock = 0.0;
        vxe::f64 cornerX = 0.0;
        vxe::f64 cornerZ = 0.0;
        Field a;
        Field b;
        Field c;
        Field d;
    };
    [[nodiscard]] Cell cell(vxe::f64 x, vxe::f64 z) const noexcept;

    // The climate at a position inside `cell`, with ground at `elevation`.
    [[nodiscard]] static ClimateSample sample(const Cell& cell, vxe::f64 x, vxe::f64 z,
                                              vxe::f64 elevation) noexcept;

    // The same for one position alone, computing its cell.
    [[nodiscard]] ClimateSample sample(vxe::f64 x, vxe::f64 z, vxe::f64 elevation) const noexcept;

    // The slow fields at one point. Far terrain blends these across a tile
    // instead of paying for each of its cells.
    [[nodiscard]] Field field(vxe::f64 x, vxe::f64 z) const noexcept;
    [[nodiscard]] vxe::f64 coolPerBlock() const noexcept;
    // The climate from such a field, with ground at `elevation`.
    [[nodiscard]] static ClimateSample fromField(const Field& field, vxe::f64 elevation,
                                                 vxe::f64 coolPerBlock) noexcept;

    // The biome that climate and ground make together.
    [[nodiscard]] static Biome biome(const PlanetSample& ground, const ClimateSample& climate) noexcept;

private:
    const Planet* m_planet;
    vxe::u64 m_warmth;
    vxe::u64 m_detail;
    vxe::u64 m_rain;
    vxe::u64 m_windX;
    vxe::u64 m_windZ;
    vxe::u64 m_variety;
};

} // namespace vr
