#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "painter.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

using texgen::Image;
using texgen::Recipe;
using texgen::Rgba;

namespace {

std::vector<Rgba> palette(int count, int alpha = 255) {
    std::vector<Rgba> colors;
    for (int i = 0; i < count; ++i) {
        const auto v = static_cast<std::uint8_t>(40 + (i * 30));
        colors.push_back({.r = v, .g = v, .b = v, .a = static_cast<std::uint8_t>(alpha)});
    }
    return colors;
}

Recipe recipe(std::string key, std::string pattern) {
    return {
        .key = std::move(key), .pattern = std::move(pattern), .palette = palette(6), .accent = palette(4)};
}

} // namespace

TEST_CASE("Colors parse from #rrggbb and #rrggbbaa") {
    Rgba color;
    REQUIRE(texgen::parseColor("#102030", color));
    CHECK(color == Rgba{.r = 0x10, .g = 0x20, .b = 0x30, .a = 255});
    REQUIRE(texgen::parseColor("#A0b0C080", color));
    CHECK(color == Rgba{.r = 0xA0, .g = 0xB0, .b = 0xC0, .a = 0x80});
    CHECK_FALSE(texgen::parseColor("102030", color));
    CHECK_FALSE(texgen::parseColor("#1020", color));
    CHECK_FALSE(texgen::parseColor("#10203g", color));
}

TEST_CASE("Every pattern paints at 16 and 32 pixels, the same every time") {
    std::vector<Recipe> bases = {recipe("stone", "stone"), recipe("dirt", "speckle")};
    for (const std::string_view name : texgen::patternNames()) {
        Recipe r = recipe("test", std::string(name));
        if (name == "ore") {
            r.base = "stone";
        } else if (name == "overhang") {
            r.base = "dirt";
        }
        for (const int size : {16, 32}) {
            Image first;
            Image second;
            std::string error;
            const bool painted = texgen::paint(r, bases, size, first, error);
            const std::string message = std::string(name) + ": " + error;
            REQUIRE_MESSAGE(painted, message);
            REQUIRE(texgen::paint(r, bases, size, second, error));
            CHECK(first.pixels == second.pixels);
            CHECK(first.pixels.size() == static_cast<std::size_t>(size * size));
        }
        // Another key gives another layout.
        Recipe other = r;
        other.key = "other";
        Image a;
        Image b;
        std::string error;
        REQUIRE(texgen::paint(r, bases, 16, a, error));
        REQUIRE(texgen::paint(other, bases, 16, b, error));
        if (name != "glass") { // Glass is a fixed design.
            CHECK_MESSAGE(a.pixels != b.pixels, name);
        }
    }
}

TEST_CASE("Leaves are cutout and glass is see through with a solid frame") {
    Image leaves;
    std::string error;
    REQUIRE(texgen::paint(recipe("leaves", "leaves"), {}, 16, leaves, error));
    std::size_t holes = 0;
    for (const Rgba& pixel : leaves.pixels) {
        CHECK((pixel.a == 0 || pixel.a == 255));
        holes += pixel.a == 0 ? 1 : 0;
    }
    CHECK(holes > 0);
    CHECK(holes < leaves.pixels.size() / 2);

    Recipe glass = recipe("glass", "glass");
    glass.alpha = 60;
    Image image;
    REQUIRE(texgen::paint(glass, {}, 16, image, error));
    CHECK(image.at(0, 5).a == 255);
    CHECK(image.at(15, 15).a == 255);
    CHECK(image.at(4, 8).a < 255);
}

TEST_CASE("Bad recipes say what is wrong") {
    Image image;
    std::string error;
    CHECK_FALSE(texgen::paint(recipe("x", "swirl"), {}, 16, image, error));
    CHECK(error.find("unknown pattern") != std::string::npos);
    Recipe ore = recipe("ore", "ore");
    CHECK_FALSE(texgen::paint(ore, {}, 16, image, error));
    CHECK(error.find("needs a base") != std::string::npos);
    ore.base = "missing";
    CHECK_FALSE(texgen::paint(ore, {}, 16, image, error));
    CHECK(error.find("not found") != std::string::npos);
    Recipe loop = recipe("loop", "ore");
    loop.base = "loop";
    const std::vector<Recipe> self = {loop};
    CHECK_FALSE(texgen::paint(loop, self, 16, image, error));
    Recipe stone = recipe("stone", "stone");
    stone.palette.resize(2);
    CHECK_FALSE(texgen::paint(stone, {}, 16, image, error));
    CHECK(error.find("at least 3 colors") != std::string::npos);
}

TEST_CASE("Textures tile: the left and right edges continue each other like the middle") {
    // Tiling noise: the color step across the wrap is no bigger than the
    // largest step between neighbors inside the image.
    Image image;
    std::string error;
    Recipe r = recipe("tile", "speckle");
    r.density = 0.0;
    REQUIRE(texgen::paint(r, {}, 16, image, error));
    int insideStep = 0;
    int wrapStep = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x + 1 < 16; ++x) {
            insideStep = std::max(insideStep, std::abs(image.at(x, y).r - image.at(x + 1, y).r));
        }
        wrapStep = std::max(wrapStep, std::abs(image.at(15, y).r - image.at(0, y).r));
    }
    CHECK(wrapStep <= insideStep);
}
