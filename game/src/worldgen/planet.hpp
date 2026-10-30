#pragma once

#include <vxe/core/types.hpp>

namespace vr {

// What decides a world's large shapes. World types are presets of these
// (G3.8). Lengths and heights are in blocks.
struct PlanetPreset {
    vxe::f64 continentSize = 14000.0; // Across a typical landmass.
    vxe::f64 landBias = 0.02;         // More land above 0, more ocean below.
    vxe::f64 plateSize = 7000.0;      // Across a tectonic plate.
    vxe::f64 rangeWidth = 1300.0;     // A mountain range's reach from its plate border.
    vxe::f64 rangeHeight = 760.0;     // Peaks of a normal range.
    vxe::f64 giantFactor = 2.0;       // How much higher the rare giants stand.
    vxe::f64 oceanDepth = 170.0;
    vxe::f64 hillHeight = 42.0;
};

// One position of the world, seen from far above. Everything further down
// the pipeline (climate, biomes, terrain) reads these fields.
struct PlanetSample {
    vxe::f64 elevation = 0.0; // Blocks above sea level.
    vxe::f64 continent = 0.0; // Below 0 is ocean, far inland about 0.5.
    vxe::f64 mountain = 0.0;  // 0 to 1: how much of a range stands here.
    vxe::f64 rift = 0.0;      // 0 to 1: how much plates pull apart here.
};

// The planet layer (G3.1): continents, tectonic plates, and mountain ranges
// where plates collide. Ranges come from plate borders, so they run long and
// purposeful instead of sitting around as noise blobs, and where plates meet
// under the sea they raise island arcs.
//
// Plates are cells of a jittered grid, each moving its own way. Two plates
// that meet head on raise a range along their border, and two that part open
// a rift. All pairs of nearby plates count, each by how close it is to being
// a real border here, so borders fade at junctions instead of ending in cuts.
//
// A pure function of the seed and position, in blocks, like all of world
// generation (Section 12.1): any position, in any order, on any thread.
//
// Thread safety: read only after construction.
class Planet {
public:
    Planet(vxe::u64 seed, const PlanetPreset& preset) noexcept;

    [[nodiscard]] PlanetSample sample(vxe::f64 x, vxe::f64 z) const noexcept;

    // How high the mountains stand here, roughly and without their crests:
    // what wind runs into. Cheaper than sample(), for looking at many places.
    [[nodiscard]] vxe::f64 barrier(vxe::f64 x, vxe::f64 z) const noexcept;

    // The continent value of PlanetSample, roughly and at the same price.
    [[nodiscard]] vxe::f64 landMass(vxe::f64 x, vxe::f64 z) const noexcept;

    [[nodiscard]] const PlanetPreset& preset() const noexcept { return m_preset; }

private:
    struct Tectonics {
        vxe::f64 range = 0.0; // 0 to 1.
        vxe::f64 rift = 0.0;  // 0 to 1.
    };
    [[nodiscard]] Tectonics tectonics(vxe::f64 x, vxe::f64 z) const noexcept;

    // The shape of the land before any crests and hills.
    struct Mass {
        vxe::f64 warpedX = 0.0;
        vxe::f64 warpedZ = 0.0;
        vxe::f64 continent = 0.0;
        vxe::f64 inland = 0.0;   // 0 at sea, 1 on land.
        vxe::f64 strength = 0.0; // 0 to 1: how hard plates push here.
        vxe::f64 tall = 0.0;     // The tallest a range stands here, in blocks.
        vxe::f64 rift = 0.0;
    };
    [[nodiscard]] Mass mass(vxe::f64 x, vxe::f64 z, vxe::u32 continentOctaves) const noexcept;

    PlanetPreset m_preset;
    vxe::u64 m_warpX;
    vxe::u64 m_warpZ;
    vxe::u64 m_continent;
    vxe::u64 m_plates;
    vxe::u64 m_ridges;
    vxe::u64 m_giants;
    vxe::u64 m_hills;
};

} // namespace vr
