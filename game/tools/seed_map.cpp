// The seed map (G3.0): a world as a picture from above, without starting the
// game, so hundreds of seeds can be judged in minutes. Colors show height,
// with light from the northwest so that ranges read as shapes.
//
//   vr_seed_map <seed> <out.png> [pixels] [blocks per pixel] [view] [center x] [center z]
//
// The view is height (default, the planet alone), terrain (with erosion),
// biomes, temperature or rain. Prints what a seed holds: land share, highest
// peak, time per sample, the share of biomes. The view chunks instead prints
// how long a chunk takes, on one core, along a walk through the world.

#include "worldgen/climate.hpp"
#include "worldgen/planet.hpp"
#include "worldgen/terrain.hpp"

#include <vxe/core/types.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using vxe::f64;
using vxe::u32;
using vxe::u8;
using vxe::usize;

struct Color {
    f64 r;
    f64 g;
    f64 b;
};

struct Stop {
    f64 height;
    Color color;
};

// Height to color, from the deep sea to the snow line.
constexpr std::array<Stop, 11> kStops = {{
    {-180.0, {16, 42, 92}},
    {-60.0, {28, 76, 140}},
    {-6.0, {62, 132, 186}},
    {0.0, {214, 202, 150}},
    {6.0, {120, 164, 88}},
    {60.0, {74, 132, 66}},
    {150.0, {112, 134, 76}},
    {320.0, {132, 112, 84}},
    {620.0, {118, 106, 100}},
    {900.0, {190, 190, 196}},
    {1500.0, {250, 250, 255}},
}};

constexpr std::array<Color, vr::kBiomeCount> kBiomeColors = {{
    {18, 44, 100},   // Deep Ocean
    {36, 88, 156},   // Ocean
    {38, 130, 170},  // Warm Ocean
    {170, 200, 226}, // Frozen Ocean
    {226, 214, 164}, // Beach
    {74, 96, 62},    // Swamp
    {244, 246, 252}, // Snowy Peaks
    {128, 118, 112}, // Rocky Peaks
    {132, 164, 104}, // Alpine Meadow
    {186, 196, 186}, // Tundra
    {150, 176, 168}, // Snowy Taiga
    {62, 104, 88},   // Taiga
    {178, 170, 104}, // Steppe
    {132, 186, 84},  // Plains
    {64, 136, 60},   // Forest
    {32, 96, 44},    // Dense Forest
    {192, 176, 80},  // Savanna
    {236, 206, 130}, // Desert
    {196, 104, 58},  // Badlands
    {24, 150, 62},   // Jungle
}};

// Cold blue through white to hot red, around 10 degrees.
Color heatColor(f64 degrees) {
    const f64 t = std::clamp((degrees - 10.0) / 30.0, -1.0, 1.0);
    return t < 0.0 ? Color{255.0 + (t * 215.0), 255.0 + (t * 155.0), 255.0}
                   : Color{255.0, 255.0 - (t * 205.0), 255.0 - (t * 225.0)};
}

// Dry yellow to wet blue.
Color rainColor(f64 rain) {
    return {236.0 - (rain * 216.0), 216.0 - (rain * 136.0), 150.0 + (rain * 70.0)};
}

Color colorOf(f64 height) {
    if (height <= kStops.front().height) {
        return kStops.front().color;
    }
    for (usize i = 1; i < kStops.size(); ++i) {
        if (height < kStops[i].height) {
            const f64 t = (height - kStops[i - 1].height) / (kStops[i].height - kStops[i - 1].height);
            const Color& a = kStops[i - 1].color;
            const Color& b = kStops[i].color;
            return {a.r + ((b.r - a.r) * t), a.g + ((b.g - a.g) * t), a.b + ((b.b - a.b) * t)};
        }
    }
    return kStops.back().color;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: vr_seed_map <seed> <out.png> [pixels] [blocks per pixel]");
        return 2;
    }
    const auto seed = std::strtoull(argv[1], nullptr, 10);
    const std::string out = argv[2];
    const int size = argc > 3 ? std::atoi(argv[3]) : 1024;
    const f64 scale = argc > 4 ? std::atof(argv[4]) : 32.0;
    const std::string view = argc > 5 ? argv[5] : "height";
    const f64 centerX = argc > 6 ? std::atof(argv[6]) : 0.0;
    const f64 centerZ = argc > 7 ? std::atof(argv[7]) : 0.0;

    const vr::Planet planet(seed, vr::PlanetPreset{});
    const vr::Climate climate(seed, planet);
    const vr::Terrain terrain(vr::TerrainBlocks{}, seed);
    if (view == "chunks") {
        // Whole columns from far below ground to high above it, as streaming
        // asks for them around a player, along a line across the world.
        auto states = std::make_unique<std::array<vxe::voxel::BlockStateId, vxe::kChunkVolume>>();
        const auto begun = std::chrono::steady_clock::now();
        usize chunks = 0;
        for (vxe::i32 step = 0; step < 400; ++step) {
            const vxe::i32 chunkX = step * 7;
            const vxe::i32 chunkZ = step * 3;
            const auto surface =
                static_cast<vxe::i32>(terrain.surfaceHeight(vxe::i64{chunkX} * 16, vxe::i64{chunkZ} * 16)) >>
                4;
            for (vxe::i32 chunkY = surface - 4; chunkY <= surface + 3; ++chunkY) {
                terrain.generate({.x = chunkX, .y = chunkY, .z = chunkZ}, *states);
                ++chunks;
            }
        }
        const std::chrono::duration<f64> taken = std::chrono::steady_clock::now() - begun;
        std::printf("%zu chunks, %.1f us per chunk\n", chunks,
                    taken.count() * 1.0e6 / static_cast<f64>(chunks));
        return 0;
    }
    const f64 half = static_cast<f64>(size) * scale * 0.5;
    std::vector<f64> heights(static_cast<usize>(size) * static_cast<usize>(size));
    std::vector<vr::ClimateSample> weather(heights.size());
    std::vector<vr::Biome> biomes(heights.size());

    const auto started = std::chrono::steady_clock::now();
    const u32 threads = std::max(1U, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (u32 worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            for (int y = static_cast<int>(worker); y < size; y += static_cast<int>(threads)) {
                for (int x = 0; x < size; ++x) {
                    const f64 wx = (static_cast<f64>(x) * scale) - half + centerX;
                    const f64 wz = (static_cast<f64>(y) * scale) - half + centerZ;
                    const usize index =
                        (static_cast<usize>(y) * static_cast<usize>(size)) + static_cast<usize>(x);
                    const vr::PlanetSample ground = planet.sample(wx, wz);
                    heights[index] = view == "terrain"
                                         ? terrain.farGround(wx, wz, scale).height - vr::Terrain::kSeaLevel
                                         : ground.elevation;
                    if (view != "height" && view != "terrain") {
                        weather[index] = climate.sample(wx, wz, ground.elevation);
                        biomes[index] = vr::Climate::biome(ground, weather[index]);
                    }
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const std::chrono::duration<f64> spent = std::chrono::steady_clock::now() - started;

    std::vector<u8> pixels(heights.size() * 3);
    usize land = 0;
    f64 highest = -1.0e30;
    f64 peakX = 0.0;
    f64 peakZ = 0.0;
    f64 deepest = 1.0e30;
    const auto at = [&](int x, int y) {
        const int cx = std::clamp(x, 0, size - 1);
        const int cy = std::clamp(y, 0, size - 1);
        return heights[(static_cast<usize>(cy) * static_cast<usize>(size)) + static_cast<usize>(cx)];
    };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const f64 height = at(x, y);
            land += height > 0.0 ? 1U : 0U;
            if (height > highest) {
                highest = height;
                peakX = (static_cast<f64>(x) * scale) - half + centerX;
                peakZ = (static_cast<f64>(y) * scale) - half + centerZ;
            }
            deepest = std::min(deepest, height);
            // Light from the northwest; the sea stays flat.
            const f64 slope = (std::max(height, 0.0) - std::max(at(x - 1, y - 1), 0.0)) / (scale * 1.41);
            const f64 light = std::clamp(1.0 + (slope * 2.2), 0.45, 1.45);
            const usize cell = (static_cast<usize>(y) * static_cast<usize>(size)) + static_cast<usize>(x);
            Color color = colorOf(height);
            if (view == "biomes") {
                color = kBiomeColors[static_cast<usize>(biomes[cell])];
            } else if (view == "temperature") {
                color = heatColor(weather[cell].temperature);
            } else if (view == "rain") {
                color = rainColor(weather[cell].rainfall);
            }
            const usize index = cell * 3;
            pixels[index] = static_cast<u8>(std::clamp(color.r * light, 0.0, 255.0));
            pixels[index + 1] = static_cast<u8>(std::clamp(color.g * light, 0.0, 255.0));
            pixels[index + 2] = static_cast<u8>(std::clamp(color.b * light, 0.0, 255.0));
        }
    }
    if (stbi_write_png(out.c_str(), size, size, 3, pixels.data(), size * 3) == 0) {
        std::printf("could not write %s\n", out.c_str());
        return 1;
    }
    std::printf(
        "seed %llu: %d km across, land %.0f%%, highest %.0f at %.0f %.0f, deepest %.0f, %.0f ns per "
        "sample on %u threads\n",
        static_cast<unsigned long long>(seed), static_cast<int>(static_cast<f64>(size) * scale / 1000.0),
        100.0 * static_cast<f64>(land) / static_cast<f64>(heights.size()), highest, peakX, peakZ, deepest,
        spent.count() * 1.0e9 * static_cast<f64>(threads) / static_cast<f64>(heights.size()), threads);
    if (view != "height" && view != "terrain") {
        std::array<usize, vr::kBiomeCount> counts{};
        for (const vr::Biome biome : biomes) {
            ++counts[static_cast<usize>(biome)];
        }
        for (usize i = 0; i < vr::kBiomeCount; ++i) {
            std::printf("  %-14.*s %5.1f%%\n", static_cast<int>(vr::kBiomeNames[i].size()),
                        vr::kBiomeNames[i].data(),
                        100.0 * static_cast<f64>(counts[i]) / static_cast<f64>(biomes.size()));
        }
    }
    return 0;
}
