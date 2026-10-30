#pragma once

#include <vxe/core/types.hpp>

namespace vr {

// What decides a world's large shapes. World types are presets of these
// (G3.8). Lengths and heights are in blocks.
struct PlanetPreset {
    vxe::f64 continentSize = 9000.0; // Across a typical landmass.
    vxe::f64 landBias = 0.1;         // More land above 0, more ocean below.
    vxe::f64 plateSize = 3400.0;     // Across a tectonic plate.
    vxe::f64 rangeWidth = 620.0;     // A mountain range's reach from its plate border.
    vxe::f64 rangeHeight = 300.0;    // The crests of a normal range.
    vxe::f64 giantFactor = 1.7;      // How much higher the rare giants stand.
    vxe::f64 peakKnee = 380.0;       // Heights above this are held back more and more...
    vxe::f64 peakCeiling = 640.0;    // ...so nothing passes this.
    vxe::f64 oceanDepth = 80.0;
    vxe::f64 inlandHeight = 26.0; // How far land climbs by itself towards the interior.
    vxe::f64 riftDepth = 14.0;
    vxe::f64 hillHeight = 64.0; // Of the roughest hills. Plains stay almost flat.
    vxe::f64 hillSize = 300.0;
    vxe::f64 valleySize = 1100.0; // How far apart winding valleys run.
    vxe::f64 valleyDepth = 26.0;
    vxe::f64 terraceHeight = 22.0; // Of one step of a plateau.
    vxe::u32 ridgeOctaves = 6;

    // Continents of about 70 km divided by oceans hundreds of kilometers wide,
    // and ranges almost 1,000 blocks tall. Grand from far above but a long
    // walk between things, so a world type of its own, never the default.
    [[nodiscard]] static constexpr PlanetPreset realistic() noexcept {
        return {.continentSize = 70000.0,
                .landBias = -0.1,
                .plateSize = 24000.0,
                .rangeWidth = 3400.0,
                .rangeHeight = 620.0,
                .giantFactor = 1.8,
                .peakKnee = 760.0,
                .peakCeiling = 1100.0,
                .oceanDepth = 300.0,
                .inlandHeight = 90.0,
                .riftDepth = 46.0,
                .hillHeight = 46.0,
                .hillSize = 1500.0,
                .valleySize = 6000.0,
                .valleyDepth = 40.0,
                .terraceHeight = 34.0,
                .ridgeOctaves = 8};
    }
};

// One position of the world, seen from far above. Everything further down
// the pipeline (climate, biomes, terrain) reads these fields.
struct PlanetSample {
    vxe::f64 elevation = 0.0; // Blocks above sea level.
    vxe::f64 continent = 0.0; // Below 0 is ocean, far inland about 0.5.
    vxe::f64 mountain = 0.0;  // 0 to 1: how much of a range stands here.
    vxe::f64 rift = 0.0;      // 0 to 1: how much plates pull apart here.
    vxe::f64 rough = 0.0;     // 0 is flat plains, 1 the roughest hills.
};

// The planet layer (G3.1): continents, tectonic plates, and mountain ranges
// where plates collide. Ranges come from plate borders, so they run long and
// purposeful instead of sitting around as noise blobs, and near the coasts
// the same collisions raise island arcs. The open ocean stays open.
//
// Land climbs gently from the coast to the interior, so the rivers of G3.5
// always find a way down to the sea. Sizes come from the preset, and a default
// world keeps things close together: see Section 27, Phase G3.
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
        vxe::f64 reach = 0.0;    // How much of a range can stand here, 0 to 1.
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
