#include "painter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace texgen {
namespace {

// SplitMix64 finalizer: well mixed bits from a few inputs, so every pixel,
// recipe and feature gets its own repeatable random values.
std::uint64_t mix(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::uint64_t hash(std::uint64_t seed, std::int64_t x, std::int64_t y, std::uint64_t salt) {
    return mix(seed ^ mix(static_cast<std::uint64_t>(x) ^ mix(static_cast<std::uint64_t>(y) ^ mix(salt))));
}

double unit(std::uint64_t h) {
    return static_cast<double>(h >> 11U) * (1.0 / 9007199254740992.0);
}

std::uint64_t keySeed(const Recipe& recipe) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : recipe.key) {
        h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001B3ULL;
    }
    return mix(h ^ recipe.seed);
}

int wrap(int value, int period) {
    const int m = value % period;
    return m < 0 ? m + period : m;
}

double smooth(double t) {
    return t * t * (3.0 - (2.0 * t));
}

// Value noise that tiles: the lattice repeats every `size` pixels, so the
// left edge continues the right one. cellX and cellY are the lattice
// spacing in pixels (different for stretched patterns like bark).
double valueNoise(std::uint64_t seed, int x, int y, int cellX, int cellY, int size) {
    cellX = std::max(cellX, 1);
    cellY = std::max(cellY, 1);
    const int periodX = std::max(1, size / cellX);
    const int periodY = std::max(1, size / cellY);
    const double gx = (static_cast<double>(x) + 0.5) / cellX;
    const double gy = (static_cast<double>(y) + 0.5) / cellY;
    const auto ix = static_cast<int>(std::floor(gx));
    const auto iy = static_cast<int>(std::floor(gy));
    const double fx = smooth(gx - ix);
    const double fy = smooth(gy - iy);
    const auto at = [&](int dx, int dy) {
        return unit(hash(seed, wrap(ix + dx, periodX), wrap(iy + dy, periodY), 1));
    };
    const double top = at(0, 0) + ((at(1, 0) - at(0, 0)) * fx);
    const double bottom = at(0, 1) + ((at(1, 1) - at(0, 1)) * fx);
    return top + ((bottom - top) * fy);
}

// Octaves of tileable noise, from `cell` pixels down to 1, 0 to 1.
double fbm(std::uint64_t seed, int x, int y, int cell, int size) {
    double sum = 0.0;
    double weight = 1.0;
    double total = 0.0;
    for (int c = std::max(cell, 1); c >= 1; c /= 2) {
        sum += valueNoise(seed + static_cast<std::uint64_t>(c), x, y, c, c, size) * weight;
        total += weight;
        weight *= 0.5;
    }
    return sum / total;
}

Rgba pick(std::span<const Rgba> palette, double t) {
    const auto count = static_cast<int>(palette.size());
    const int index = std::clamp(static_cast<int>(t * count), 0, count - 1);
    return palette[static_cast<std::size_t>(index)];
}

double jitter(std::uint64_t seed, int x, int y, std::uint64_t salt, double amount) {
    return (unit(hash(seed, x, y, salt)) - 0.5) * amount;
}

int cellFor(const Recipe& recipe, int size, int baseCell) {
    return std::clamp(static_cast<int>(std::lround(baseCell * recipe.scale * size / 16.0)), 1, size);
}

// Speckled ground (dirt, sand, snow): soft noise across the palette with a
// sprinkle of the darkest and lightest colors.
void paintSpeckle(const Recipe& r, std::uint64_t seed, Image& img) {
    const int cell = cellFor(r, img.size, 4);
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            const double t = (fbm(seed, x, y, cell, img.size) * 0.8) + 0.1 + jitter(seed, x, y, 2, 0.35);
            Rgba color = pick(r.palette, t);
            const double roll = unit(hash(seed, x, y, 3));
            if (roll < r.density * 0.08) {
                color = r.palette.front();
            } else if (roll > 1.0 - (r.density * 0.06)) {
                color = r.palette.back();
            }
            color.a = r.alpha;
            img.at(x, y) = color;
        }
    }
}

// Stone: mottled noise with a few short dark cracks.
void paintStone(const Recipe& r, std::uint64_t seed, Image& img) {
    const int cell = cellFor(r, img.size, 8);
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            const double t = (fbm(seed, x, y, cell, img.size) * 0.9) + 0.05 + jitter(seed, x, y, 2, 0.2);
            img.at(x, y) = pick(std::span(r.palette).subspan(1), t);
        }
    }
    const int cracks = 2 + static_cast<int>(r.density * 3.0);
    for (int c = 0; c < cracks; ++c) {
        int x = static_cast<int>(unit(hash(seed, c, 0, 10)) * img.size);
        int y = static_cast<int>(unit(hash(seed, c, 0, 11)) * img.size);
        const int length = 3 + static_cast<int>(unit(hash(seed, c, 0, 12)) * 5.0);
        for (int step = 0; step < length; ++step) {
            img.at(wrap(x, img.size), wrap(y, img.size)) = r.palette.front();
            // Cracks wander: right, left, or (half the time) down.
            const auto turn = static_cast<int>(unit(hash(seed, c, step, 13)) * 4.0);
            if (turn == 0) {
                ++x;
            } else if (turn == 1) {
                --x;
            } else {
                ++y;
            }
        }
    }
}

// Grass from above: speckled greens with bright and dark blade tips.
void paintGrassTop(const Recipe& r, std::uint64_t seed, Image& img) {
    const int cell = cellFor(r, img.size, 4);
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            const double t = (fbm(seed, x, y, cell, img.size) * 0.6) + 0.2 + jitter(seed, x, y, 2, 0.4);
            Rgba color = pick(r.palette, t);
            const double roll = unit(hash(seed, x, y, 3));
            if (roll < r.density * 0.2) {
                color = r.palette.back();
            } else if (roll > 0.94) {
                color = r.palette.front();
            }
            img.at(x, y) = color;
        }
    }
}

// The side of a grass block: the base (dirt) with grass hanging over the top
// edge, a ragged 2 to 5 pixels deep.
void paintOverhang(const Recipe& r, std::uint64_t seed, Image& img) {
    const int unitDepth = std::max(1, img.size / 8);
    for (int x = 0; x < img.size; ++x) {
        int depth =
            (2 * unitDepth) + static_cast<int>(valueNoise(seed, x, 0, 4, 4, img.size) * 2.0 * unitDepth);
        if (unit(hash(seed, x, 0, 20)) < r.density * 0.3) {
            depth += unitDepth; // A drip.
        }
        for (int y = 0; y < depth && y < img.size; ++y) {
            const double t = 0.55 - (static_cast<double>(y) / depth * 0.4) + jitter(seed, x, y, 21, 0.4);
            img.at(x, y) = pick(r.accent, t);
        }
    }
}

// Gravel: round pebbles (cells around random points, wrapping) with dark gaps.
void paintGravel(const Recipe& r, std::uint64_t seed, Image& img) {
    const int points = 4 + static_cast<int>(r.density * 10.0 * img.size * img.size / 256.0);
    std::vector<std::array<double, 2>> centers;
    centers.reserve(static_cast<std::size_t>(points));
    for (int i = 0; i < points; ++i) {
        centers.push_back({unit(hash(seed, i, 0, 30)) * img.size, unit(hash(seed, i, 0, 31)) * img.size});
    }
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            double nearest = 1e9;
            double second = 1e9;
            int owner = 0;
            for (int i = 0; i < points; ++i) {
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        const double dx =
                            x + 0.5 - (centers[static_cast<std::size_t>(i)][0] + (ox * img.size));
                        const double dy =
                            y + 0.5 - (centers[static_cast<std::size_t>(i)][1] + (oy * img.size));
                        const double d = std::sqrt((dx * dx) + (dy * dy));
                        if (d < nearest) {
                            second = nearest;
                            nearest = d;
                            owner = i;
                        } else if (d < second) {
                            second = d;
                        }
                    }
                }
            }
            if (second - nearest < 0.9) {
                img.at(x, y) = r.palette.front();
                continue;
            }
            // Each pebble its own shade, lighter towards its middle.
            const double t = (unit(hash(seed, owner, 0, 32)) * 0.7) + 0.3 - (nearest / (img.size * 0.4));
            img.at(x, y) = pick(std::span(r.palette).subspan(1), t);
        }
    }
}

// Water: gentle diagonal waves (whole periods across the tile, so it tiles).
void paintWater(const Recipe& r, std::uint64_t seed, Image& img) {
    constexpr double kTau = 2.0 * std::numbers::pi;
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            const double wave = std::sin(kTau * ((2.0 * y) + x) / img.size) * 0.3;
            const double t = 0.5 + wave + ((fbm(seed, x, y, 4, img.size) - 0.5) * 0.4);
            Rgba color = pick(r.palette, t);
            color.a = r.alpha;
            img.at(x, y) = color;
        }
    }
}

// Bark: vertical plates split by dark grooves that run down the trunk with
// a few breaks, and a little shading across each plate.
void paintBark(const Recipe& r, std::uint64_t seed, Image& img) {
    const int grooveCell = std::max(1, img.size / 8);
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            const double plate = valueNoise(seed, x, y, grooveCell, img.size / 2, img.size);
            const double grain = jitter(seed, x, y, 2, 0.25);
            const bool groove = plate < 0.3 + (r.density * 0.1) &&
                                unit(hash(seed, x, y / std::max(1, img.size / 4), 3)) > 0.15;
            if (groove) {
                img.at(x, y) = r.palette.front();
                continue;
            }
            img.at(x, y) = pick(std::span(r.palette).subspan(1), ((plate - 0.3) / 0.7) + grain);
        }
    }
}

// The cut end of a log: rings around the middle, bark (accent) at the edge.
void paintRings(const Recipe& r, std::uint64_t seed, Image& img) {
    const double middle = (img.size - 1) / 2.0;
    const double ringWidth = std::max(1.0, img.size / 8.0 * r.scale);
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            if (x == 0 || y == 0 || x == img.size - 1 || y == img.size - 1) {
                img.at(x, y) = pick(r.accent, 0.5 + jitter(seed, x, y, 40, 0.8));
                continue;
            }
            const double d = std::hypot(x - middle, y - middle);
            const double ring =
                std::fmod((d / ringWidth) + (valueNoise(seed, x, y, 4, 4, img.size) * 0.5), 1.0);
            img.at(x, y) = pick(r.palette, (ring * 0.7) + 0.3 - (d / img.size * 0.4));
        }
    }
}

// Leaves: clusters of leaf colors with see through gaps (alpha 0, cutout).
// Leaves right under or right of a gap are darker, which reads as depth.
void paintLeaves(const Recipe& r, std::uint64_t seed, Image& img) {
    const int cell = cellFor(r, img.size, 4);
    const auto isHole = [&](int x, int y) {
        x = wrap(x, img.size);
        y = wrap(y, img.size);
        return fbm(seed, x, y, cell, img.size) < 0.33 || unit(hash(seed, x, y, 50)) < r.density * 0.3;
    };
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            if (isHole(x, y)) {
                img.at(x, y) = {.r = 0, .g = 0, .b = 0, .a = 0};
                continue;
            }
            double t = fbm(seed + 7, x, y, cell, img.size) + jitter(seed, x, y, 51, 0.5);
            if (isHole(x, y - 1) || isHole(x - 1, y)) {
                t -= 0.45;
            } else if (isHole(x, y + 1) || isHole(x + 1, y)) {
                t += 0.25;
            }
            Rgba color = pick(r.palette, t);
            color.a = 255;
            img.at(x, y) = color;
        }
    }
}

// Glass: a light frame, a nearly clear middle and two diagonal glints.
void paintGlass(const Recipe& r, std::uint64_t /*seed*/, Image& img) {
    const int last = img.size - 1;
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            Rgba color = r.palette.front();
            color.a = r.alpha;
            const bool frame = x == 0 || y == 0 || x == last || y == last;
            const int diagonal = wrap(x + y, img.size);
            const bool glint =
                x > 1 && y > 1 && x < last - 1 && y < last - 1 &&
                (diagonal == img.size / 2 || diagonal == (img.size / 2) + 1 || diagonal == img.size / 4);
            if (frame) {
                color = r.palette.back();
                color.a = 255;
            } else if (glint) {
                color = r.palette[r.palette.size() / 2];
                color.a = static_cast<std::uint8_t>(std::min(255, r.alpha * 3));
            }
            img.at(x, y) = color;
        }
    }
}

// A glowing block: four bright panes in a darker frame, brightest in the middle.
void paintGlow(const Recipe& r, std::uint64_t seed, Image& img) {
    const int pane = img.size / 2;
    const double middle = (img.size - 1) / 2.0;
    for (int y = 0; y < img.size; ++y) {
        for (int x = 0; x < img.size; ++x) {
            const bool grout = x % pane == 0 || y % pane == 0;
            if (grout) {
                img.at(x, y) = r.palette.front();
                continue;
            }
            const double glow = 1.0 - (std::hypot(x - middle, y - middle) / (img.size * 0.75));
            const double t = (glow * 0.7) + (fbm(seed, x, y, 4, img.size) * 0.3);
            img.at(x, y) = pick(std::span(r.palette).subspan(1), t);
        }
    }
}

// Ore: lumps of the accent colors over the base (stone): each lump grows
// from a seed pixel into its neighbors, lit from the top left (lightest
// accent) and shaded towards the bottom right (darkest).
void paintOre(const Recipe& r, std::uint64_t seed, Image& img) {
    const int lumps = 3 + static_cast<int>(r.density * 6.0 * img.size * img.size / 256.0);
    for (int c = 0; c < lumps; ++c) {
        const auto cx = static_cast<int>(unit(hash(seed, c, 0, 60)) * img.size);
        const auto cy = static_cast<int>(unit(hash(seed, c, 0, 61)) * img.size);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const bool centre = dx == 0 && dy == 0;
                if (!centre && unit(hash(seed, c, ((dy + 1) * 3) + dx + 1, 62)) > 0.7) {
                    continue;
                }
                const double light = 0.5 - ((dx + dy) * 0.22) + jitter(seed, c, (dy * 3) + dx, 63, 0.2);
                img.at(wrap(cx + dx, img.size), wrap(cy + dy, img.size)) = pick(r.accent, light);
            }
        }
    }
}

struct Pattern {
    std::string_view name;
    void (*paint)(const Recipe&, std::uint64_t, Image&);
    std::size_t minPalette;
    bool needsAccent;
    bool needsBase;
};

constexpr std::array<Pattern, 12> kPatterns = {{
    {.name = "speckle", .paint = paintSpeckle, .minPalette = 2, .needsAccent = false, .needsBase = false},
    {.name = "stone", .paint = paintStone, .minPalette = 3, .needsAccent = false, .needsBase = false},
    {.name = "grass_top", .paint = paintGrassTop, .minPalette = 2, .needsAccent = false, .needsBase = false},
    {.name = "overhang", .paint = paintOverhang, .minPalette = 0, .needsAccent = true, .needsBase = true},
    {.name = "gravel", .paint = paintGravel, .minPalette = 3, .needsAccent = false, .needsBase = false},
    {.name = "water", .paint = paintWater, .minPalette = 2, .needsAccent = false, .needsBase = false},
    {.name = "bark", .paint = paintBark, .minPalette = 2, .needsAccent = false, .needsBase = false},
    {.name = "rings", .paint = paintRings, .minPalette = 2, .needsAccent = true, .needsBase = false},
    {.name = "leaves", .paint = paintLeaves, .minPalette = 2, .needsAccent = false, .needsBase = false},
    {.name = "glass", .paint = paintGlass, .minPalette = 2, .needsAccent = false, .needsBase = false},
    {.name = "glow", .paint = paintGlow, .minPalette = 3, .needsAccent = false, .needsBase = false},
    {.name = "ore", .paint = paintOre, .minPalette = 0, .needsAccent = true, .needsBase = true},
}};

constexpr std::array<std::string_view, kPatterns.size()> kPatternNames = [] {
    std::array<std::string_view, kPatterns.size()> names{};
    for (std::size_t i = 0; i < kPatterns.size(); ++i) {
        names[i] = kPatterns[i].name;
    }
    return names;
}();

// Bases nest (an ore on stone); `depth` stops a loop of bases after a few levels.
// NOLINTNEXTLINE(misc-no-recursion): bounded by depth.
bool paintInto(const Recipe& recipe, std::span<const Recipe> bases, Image& out, std::string& error,
               int depth) {
    const auto pattern = std::ranges::find(kPatterns, recipe.pattern, &Pattern::name);
    if (pattern == kPatterns.end()) {
        error = "unknown pattern '" + recipe.pattern + "'";
        return false;
    }
    if (recipe.palette.size() < pattern->minPalette) {
        error = "pattern '" + recipe.pattern + "' needs at least " + std::to_string(pattern->minPalette) +
                " colors";
        return false;
    }
    if (pattern->needsAccent && recipe.accent.empty()) {
        error = "pattern '" + recipe.pattern + "' needs accent colors";
        return false;
    }
    if (pattern->needsBase || !recipe.base.empty()) {
        const auto base = std::ranges::find(bases, recipe.base, &Recipe::key);
        if (base == bases.end() || depth > 4) {
            error = recipe.base.empty() ? "pattern '" + recipe.pattern + "' needs a base"
                                        : "base '" + recipe.base + "' not found (or bases loop)";
            return false;
        }
        if (!paintInto(*base, bases, out, error, depth + 1)) {
            error = "base '" + recipe.base + "': " + error;
            return false;
        }
    }
    pattern->paint(recipe, keySeed(recipe), out);
    return true;
}

} // namespace

bool parseColor(std::string_view text, Rgba& out) {
    if (text.size() != 7 && text.size() != 9) {
        return false;
    }
    if (text.front() != '#') {
        return false;
    }
    std::array<std::uint8_t, 4> channels = {0, 0, 0, 255};
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        unsigned value = 0;
        for (std::size_t j = 1; j <= 2; ++j) {
            const char c = text[i + j];
            value *= 16;
            if (c >= '0' && c <= '9') {
                value += static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value += static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value += static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        channels[i / 2] = static_cast<std::uint8_t>(value);
    }
    out = {.r = channels[0], .g = channels[1], .b = channels[2], .a = channels[3]};
    return true;
}

std::span<const std::string_view> patternNames() {
    return kPatternNames;
}

bool paint(const Recipe& recipe, std::span<const Recipe> bases, int size, Image& out, std::string& error) {
    out.size = size;
    out.pixels.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), Rgba{});
    return paintInto(recipe, bases, out, error, 0);
}

Image upscale(const Image& image, int factor) {
    Image out;
    out.size = image.size * factor;
    out.pixels.resize(static_cast<std::size_t>(out.size) * static_cast<std::size_t>(out.size));
    for (int y = 0; y < out.size; ++y) {
        for (int x = 0; x < out.size; ++x) {
            out.at(x, y) = image.at(x / factor, y / factor);
        }
    }
    return out;
}

} // namespace texgen
