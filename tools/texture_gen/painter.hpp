#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The texture generator's painter (G2.1): turns a recipe (a pattern, a
// palette and a few numbers) into a small square RGBA image. Everything is
// deterministic for a seed and tiles seamlessly, so generated block faces
// repeat without visible edges. New materials are new recipes; new looks are
// new patterns in painter.cpp.
namespace texgen {

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    friend constexpr bool operator==(const Rgba&, const Rgba&) = default;
};

// "#rrggbb" or "#rrggbbaa"; false if the text is not a color.
bool parseColor(std::string_view text, Rgba& out);

// One texture to paint. Palettes go from dark to light.
struct Recipe {
    std::string key{};           // The texture key and file name (blocks/<key>.png).
    std::string pattern{};       // See patternNames().
    std::vector<Rgba> palette{}; // The main colors, dark to light.
    std::vector<Rgba> accent{};  // A second palette: grass over dirt, ore over stone, bark around rings.
    std::string base{};          // Optional: paint this recipe first (ores on stone, grass sides on dirt).
    double density = 0.5;        // Pattern specific: how much of the accent, speckles or holes.
    double scale = 1.0;          // Pattern specific: feature size.
    std::uint8_t alpha = 255;    // For translucent patterns (water, glass).
    std::uint32_t seed = 0;      // Mixed with the key, so recipes differ even with equal seeds.
};

// An image, rows from the top (as PNG files store them).
struct Image {
    int size = 16;
    std::vector<Rgba> pixels; // size * size.

    [[nodiscard]] Rgba& at(int x, int y) { return pixels[static_cast<std::size_t>((y * size) + x)]; }
    [[nodiscard]] const Rgba& at(int x, int y) const {
        return pixels[static_cast<std::size_t>((y * size) + x)];
    }
};

// The patterns paint() knows.
[[nodiscard]] std::span<const std::string_view> patternNames();

// Paints `recipe` at `size` pixels. `bases` resolves base recipes by key.
// Fails (returns false, with a message) on an unknown pattern, a missing base
// or a palette too short for the pattern.
bool paint(const Recipe& recipe, std::span<const Recipe> bases, int size, Image& out, std::string& error);

// The nearest neighbor upscale of an image, for previews and icons.
[[nodiscard]] Image upscale(const Image& image, int factor);

} // namespace texgen
