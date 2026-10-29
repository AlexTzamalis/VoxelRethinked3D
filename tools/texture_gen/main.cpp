// texture_gen (G2.1): paints the default texture pack from recipes.
//
//   vxe_texture_gen <recipes.toml> <pack folder> [--force] [--only key,key]
//                   [--preview file.png] [--template folder]
//
// Writes <pack>/blocks/<key>.png for every recipe, <pack>/pack.toml and
// <pack>/pack.png (the icon). Existing files are kept unless --force is
// given, so textures edited by hand (Aseprite) survive a run; --only limits
// the run to some keys. --preview writes every texture enlarged on one sheet
// to look at. --template copies the pack as it is now (hand edits included)
// into a new folder, renamed "My Pack", as the start of a new pack.

#include "painter.hpp"

#include <toml++/toml.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using texgen::Image;
using texgen::Recipe;
using texgen::Rgba;

namespace {

struct PackInfo {
    std::string name = "Default";
    std::string author;
    std::string version = "1.0.0";
    std::string description;
    int resolution = 16;
    std::uint32_t seed = 1;
    std::vector<std::string> icon; // Four keys, shown 2 x 2.
};

struct Options { // NOLINT(bugprone-exception-escape): a throwing string ends the tool anyway.
    fs::path recipes;
    fs::path pack;
    bool force = false;
    std::set<std::string> only;
    std::optional<fs::path> preview;
    std::optional<fs::path> templateFolder;
};

void fail(const std::string& message) {
    std::fputs(("texture_gen: " + message + "\n").c_str(), stderr);
}

bool readColors(const toml::array* array, std::vector<Rgba>& out, const std::string& where) {
    if (array == nullptr) {
        return true;
    }
    for (const toml::node& node : *array) {
        Rgba color;
        const std::optional<std::string> text = node.value<std::string>();
        if (!text || !texgen::parseColor(*text, color)) {
            fail(where + R"(: colors are "#rrggbb" or "#rrggbbaa")");
            return false;
        }
        out.push_back(color);
    }
    return true;
}

bool loadRecipes(const fs::path& path, PackInfo& pack, std::vector<Recipe>& recipes) {
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        fail(std::format("{}: {}", path.string(), error.description()));
        return false;
    }
    if (const toml::table* info = root["pack"].as_table()) {
        pack.name = (*info)["name"].value_or(pack.name);
        pack.author = (*info)["author"].value_or(pack.author);
        pack.version = (*info)["version"].value_or(pack.version);
        pack.description = (*info)["description"].value_or(pack.description);
        pack.resolution = (*info)["resolution"].value_or(pack.resolution);
        pack.seed = (*info)["seed"].value_or(pack.seed);
        if (const toml::array* icon = (*info)["icon"].as_array()) {
            for (const toml::node& key : *icon) {
                pack.icon.push_back(key.value_or(std::string()));
            }
        }
    }
    const toml::array* textures = root["texture"].as_array();
    if (textures == nullptr) {
        fail(path.string() + ": no [[texture]] entries");
        return false;
    }
    for (const toml::node& node : *textures) {
        const toml::table* entry = node.as_table();
        if (entry == nullptr) {
            continue;
        }
        Recipe recipe;
        recipe.key = (*entry)["key"].value_or(std::string());
        recipe.pattern = (*entry)["pattern"].value_or(std::string());
        recipe.base = (*entry)["base"].value_or(std::string());
        recipe.density = (*entry)["density"].value_or(recipe.density);
        recipe.scale = (*entry)["scale"].value_or(recipe.scale);
        recipe.alpha = static_cast<std::uint8_t>((*entry)["alpha"].value_or(255));
        recipe.seed = pack.seed + (*entry)["seed"].value_or(0U);
        const std::string where = std::format("{}: texture '{}'", path.string(), recipe.key);
        if (recipe.key.empty() || recipe.pattern.empty()) {
            fail(where + " needs a key and a pattern");
            return false;
        }
        if (!readColors((*entry)["palette"].as_array(), recipe.palette, where) ||
            !readColors((*entry)["accent"].as_array(), recipe.accent, where)) {
            return false;
        }
        recipes.push_back(std::move(recipe));
    }
    return true;
}

bool writePng(const fs::path& path, const Image& image) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const int written =
        stbi_write_png(path.string().c_str(), image.size, image.size, 4, image.pixels.data(), image.size * 4);
    if (written == 0) {
        fail("could not write " + path.string());
    }
    return written != 0;
}

std::string packToml(const PackInfo& pack) {
    std::ostringstream out;
    out << "# Every pack needs this file in its root folder (Section 15.2).\n";
    out << "name        = \"" << pack.name << "\"\n";
    out << "author      = \"" << pack.author << "\"\n";
    out << "version     = \"" << pack.version << "\"\n";
    out << "description = \"" << pack.description << "\"\n";
    out << "format      = 1          # Pack format version.\n";
    out << "resolution  = " << pack.resolution << "         # Pixel size of block textures.\n\n";
    out << "[options]\n";
    out << "mipmap_alpha_preserve = true   # Keeps leaves from disappearing at distance.\n";
    return out.str();
}

// A sheet of every texture enlarged 8 times on a checkerboard (so
// transparency shows), 4 per row, in recipe order.
Image previewSheet(const std::vector<Image>& images) {
    constexpr int kFactor = 8;
    constexpr int kGap = 8;
    constexpr int kColumns = 4;
    const int cell = (images.empty() ? 16 : images.front().size) * kFactor;
    const int rows = (static_cast<int>(images.size()) + kColumns - 1) / kColumns;
    Image sheet;
    sheet.size = std::max(kColumns, rows) * (cell + kGap) + kGap;
    sheet.pixels.resize(static_cast<std::size_t>(sheet.size) * static_cast<std::size_t>(sheet.size));
    for (int y = 0; y < sheet.size; ++y) {
        for (int x = 0; x < sheet.size; ++x) {
            const bool light = ((x / 8) + (y / 8)) % 2 == 0;
            sheet.at(x, y) = light ? Rgba{.r = 200, .g = 200, .b = 200} : Rgba{.r = 150, .g = 150, .b = 150};
        }
    }
    for (std::size_t i = 0; i < images.size(); ++i) {
        const Image big = texgen::upscale(images[i], kFactor);
        const int left = kGap + (static_cast<int>(i % kColumns) * (cell + kGap));
        const int top = kGap + (static_cast<int>(i / kColumns) * (cell + kGap));
        for (int y = 0; y < big.size; ++y) {
            for (int x = 0; x < big.size; ++x) {
                const Rgba src = big.at(x, y);
                Rgba& dst = sheet.at(left + x, top + y);
                const int a = src.a;
                dst.r = static_cast<std::uint8_t>(((src.r * a) + (dst.r * (255 - a))) / 255);
                dst.g = static_cast<std::uint8_t>(((src.g * a) + (dst.g * (255 - a))) / 255);
                dst.b = static_cast<std::uint8_t>(((src.b * a) + (dst.b * (255 - a))) / 255);
                dst.a = 255;
            }
        }
    }
    return sheet;
}

// The pack icon: four textures, 2 x 2, each at twice its size.
Image iconImage(const PackInfo& pack, const std::vector<Recipe>& recipes, const std::vector<Image>& images) {
    const int half = pack.resolution * 2;
    Image icon;
    icon.size = half * 2;
    icon.pixels.resize(static_cast<std::size_t>(icon.size) * static_cast<std::size_t>(icon.size));
    for (std::size_t i = 0; i < 4 && i < pack.icon.size(); ++i) {
        const auto found = std::ranges::find(recipes, pack.icon[i], &Recipe::key);
        if (found == recipes.end()) {
            continue;
        }
        const Image big = texgen::upscale(images[static_cast<std::size_t>(found - recipes.begin())], 2);
        const int left = static_cast<int>(i % 2) * half;
        const int top = static_cast<int>(i / 2) * half;
        for (int y = 0; y < half; ++y) {
            for (int x = 0; x < half; ++x) {
                icon.at(left + x, top + y) = big.at(x, y);
            }
        }
    }
    return icon;
}

bool writeTextFile(const fs::path& path, const std::string& text) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path, std::ios::binary);
    file << text;
    return static_cast<bool>(file);
}

// A copy of the pack as it is now, renamed, for a new pack to start from.
bool writeTemplate(const fs::path& pack, const fs::path& target, PackInfo info,
                   const std::vector<Recipe>& recipes) {
    std::error_code error;
    if (fs::exists(target, error)) {
        fail(target.string() + " exists already; pick a new folder for the template");
        return false;
    }
    fs::create_directories(target / "blocks", error);
    for (const Recipe& recipe : recipes) {
        const fs::path source = pack / "blocks" / (recipe.key + ".png");
        fs::copy_file(source, target / "blocks" / (recipe.key + ".png"), error);
        if (error) {
            fail("could not copy " + source.string() + ": " + error.message());
            return false;
        }
    }
    fs::copy_file(pack / "pack.png", target / "pack.png", error);
    info.name = "My Pack";
    info.author = "";
    info.description = "Made from the default pack template.";
    std::string keys = "Texture keys of this pack (blocks/<key>.png, " + std::to_string(info.resolution) +
                       " x " + std::to_string(info.resolution) + " pixels, any size up to 256 works):\n";
    for (const Recipe& recipe : recipes) {
        keys += "  " + recipe.key + "\n";
    }
    return writeTextFile(target / "pack.toml", packToml(info)) && writeTextFile(target / "KEYS.txt", keys);
}

std::optional<Options> parseArguments(int argc, char** argv) {
    Options options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool hasValue = i + 1 < argc;
        if (argument == "--force") {
            options.force = true;
        } else if (argument == "--only" && hasValue) {
            const std::string list = argv[++i];
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t comma = std::min(list.find(',', start), list.size());
                options.only.insert(list.substr(start, comma - start));
                start = comma + 1;
            }
        } else if (argument == "--preview" && hasValue) {
            options.preview = argv[++i];
        } else if (argument == "--template" && hasValue) {
            options.templateFolder = argv[++i];
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 2) {
        fail("usage: vxe_texture_gen <recipes.toml> <pack folder> [--force] [--only key,key] "
             "[--preview file.png] [--template folder]");
        return std::nullopt;
    }
    options.recipes = positional[0];
    options.pack = positional[1];
    return options;
}

} // namespace

namespace {

int run(int argc, char** argv) {
    const std::optional<Options> options = parseArguments(argc, argv);
    if (!options) {
        return 2;
    }
    PackInfo pack;
    std::vector<Recipe> recipes;
    if (!loadRecipes(options->recipes, pack, recipes)) {
        return 1;
    }
    std::vector<Image> images(recipes.size());
    int written = 0;
    int kept = 0;
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        std::string error;
        if (!texgen::paint(recipes[i], recipes, pack.resolution, images[i], error)) {
            fail("texture '" + recipes[i].key + "': " + error);
            return 1;
        }
        if (!options->only.empty() && !options->only.contains(recipes[i].key)) {
            continue;
        }
        const fs::path file = options->pack / "blocks" / (recipes[i].key + ".png");
        std::error_code exists;
        if (!options->force && fs::exists(file, exists)) {
            ++kept;
            continue;
        }
        if (!writePng(file, images[i])) {
            return 1;
        }
        ++written;
    }
    std::error_code exists;
    if (options->force || !fs::exists(options->pack / "pack.toml", exists)) {
        if (!writeTextFile(options->pack / "pack.toml", packToml(pack))) {
            fail("could not write pack.toml");
            return 1;
        }
    }
    if (options->force || !fs::exists(options->pack / "pack.png", exists)) {
        if (!writePng(options->pack / "pack.png", iconImage(pack, recipes, images))) {
            return 1;
        }
    }
    std::fputs(
        std::format("texture_gen: {} textures written, {} kept (existing; --force repaints)\n", written, kept)
            .c_str(),
        stdout);
    if (options->preview && !writePng(*options->preview, previewSheet(images))) {
        return 1;
    }
    if (options->templateFolder && !writeTemplate(options->pack, *options->templateFolder, pack, recipes)) {
        return 1;
    }
    return 0;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): reporting a failure may throw, which ends the tool as well.
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        fail(error.what());
    } catch (...) {
        fail("unknown failure");
    }
    return 1;
}
