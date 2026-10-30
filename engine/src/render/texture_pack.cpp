#include "vxe/render/texture_pack.hpp"

#include <vxe/core/profile.hpp>
#include <vxe/io/file_io.hpp>

#include <toml++/toml.hpp>

// stb_image decodes the PNGs; only PNG is needed, and the decoder is
// compiled here once.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // The menu panorama: photo like, far smaller than a PNG.
#define STBI_NO_STDIO
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <sstream>
#include <utility>

namespace vxe::render {

std::string textureFile(std::string_view key) {
    return key.find('/') == std::string_view::npos ? std::format("blocks/{}", key) : std::string(key);
}
namespace {

constexpr u32 kMagenta = 0xFFFF00FFU;
constexpr u32 kBlack = 0xFF000000U;

u32 channel(u32 pixel, u32 index) noexcept {
    return (pixel >> (index * 8U)) & 0xFFU;
}

class FolderPack final : public PackSource {
public:
    FolderPack(std::filesystem::path folder, PackManifest manifest)
        : m_folder(std::move(folder))
        , m_manifest(std::move(manifest)) {}

    [[nodiscard]] const PackManifest& manifest() const noexcept override { return m_manifest; }
    [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const override {
        auto bytes = io::readFile(m_folder / std::filesystem::path(path));
        if (!bytes) {
            return std::nullopt;
        }
        return std::move(bytes.value());
    }

private:
    std::filesystem::path m_folder;
    PackManifest m_manifest;
};

class EmbeddedPack final : public PackSource {
public:
    EmbeddedPack(PackManifest manifest, std::span<const EmbeddedFile> files)
        : m_manifest(std::move(manifest)) {
        for (const EmbeddedFile& file : files) {
            m_files.emplace(std::string(file.path), file.bytes);
        }
    }

    [[nodiscard]] const PackManifest& manifest() const noexcept override { return m_manifest; }
    [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const override {
        const auto it = m_files.find(std::string(path));
        if (it == m_files.end()) {
            return std::nullopt;
        }
        const auto bytes = std::as_bytes(it->second);
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    }

private:
    PackManifest m_manifest;
    std::map<std::string, std::span<const unsigned char>> m_files;
};

std::string_view asText(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()),
            bytes.size()}; // NOLINT(*-reinterpret-cast): bytes as text.
}

// One decoded image, rows from the top as in the file.
struct Decoded {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> pixels;
};

std::optional<Decoded> decodeImage(std::span<const std::byte> bytes) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), // NOLINT(*-reinterpret-cast)
                              static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (data == nullptr || width <= 0 || height <= 0) {
        stbi_image_free(data);
        return std::nullopt;
    }
    Decoded image{.width = static_cast<u32>(width),
                  .height = static_cast<u32>(height),
                  .pixels = std::vector<u32>(static_cast<usize>(width) * static_cast<usize>(height))};
    std::memcpy(image.pixels.data(), data, image.pixels.size() * sizeof(u32));
    stbi_image_free(data);
    return image;
}

// Square frame `frame` of a vertical strip, scaled to `size` with nearest
// filtering and flipped to bottom row first (ADR-016).
std::vector<u32> frameAt(const Decoded& image, u32 frame, u32 size) {
    const u32 side = image.width;
    std::vector<u32> out(usize{size} * size);
    for (u32 y = 0; y < size; ++y) {
        const u32 sourceY = (frame * side) + ((size - 1 - y) * side / size);
        for (u32 x = 0; x < size; ++x) {
            out[(usize{y} * size) + x] = image.pixels[(usize{sourceY} * side) + (x * side / size)];
        }
    }
    return out;
}

std::vector<u32> checker(u32 size) {
    std::vector<u32> out(usize{size} * size);
    const u32 square = std::max(1U, size / 2);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            out[(usize{y} * size) + x] = ((x / square) + (y / square)) % 2 == 0 ? kMagenta : kBlack;
        }
    }
    return out;
}

// Cutout textures have only fully clear and fully solid pixels (leaves);
// translucent ones (water, glass) have alpha in between.
bool isCutout(std::span<const u32> pixels) {
    bool anyClear = false;
    for (const u32 pixel : pixels) {
        const u32 alpha = channel(pixel, 3);
        if (alpha != 0 && alpha != 255) {
            return false;
        }
        anyClear = anyClear || alpha == 0;
    }
    return anyClear;
}

f64 coverage(std::span<const u32> pixels, f64 alphaScale) {
    usize visible = 0;
    for (const u32 pixel : pixels) {
        visible += static_cast<f64>(channel(pixel, 3)) * alphaScale >= 128.0 ? 1U : 0U;
    }
    return static_cast<f64>(visible) / static_cast<f64>(pixels.size());
}

// The next mip level: 2 x 2 averages. Colors are weighted by alpha, so clear
// pixels (often black) do not darken the edges of leaves.
std::vector<u32> halve(std::span<const u32> pixels, u32 size) {
    const u32 next = size / 2;
    std::vector<u32> out(usize{next} * next);
    for (u32 y = 0; y < next; ++y) {
        for (u32 x = 0; x < next; ++x) {
            const auto at = [&](u32 dx, u32 dy) {
                return pixels[(usize{(2 * y) + dy} * size) + (usize{2} * x) + dx];
            };
            const std::array<u32, 4> texels = {at(0, 0), at(1, 0), at(0, 1), at(1, 1)};
            u32 alphaSum = 0;
            std::array<u32, 3> color{};
            for (const u32 texel : texels) {
                const u32 alpha = channel(texel, 3);
                alphaSum += alpha;
                for (u32 c = 0; c < 3; ++c) {
                    color.at(c) += channel(texel, c) * alpha;
                }
            }
            u32 pixel = (alphaSum / 4) << 24U;
            for (u32 c = 0; c < 3; ++c) {
                const u32 value = alphaSum == 0 ? 0 : color.at(c) / alphaSum;
                pixel |= std::min(value, 255U) << (c * 8U);
            }
            out[(usize{y} * next) + x] = pixel;
        }
    }
    return out;
}

// Scales alpha so the share of visible pixels matches `target` (alpha
// coverage preservation): without it, averaging leaves with gaps drives
// alpha below the cutout threshold and distant trees go bare.
void preserveCoverage(std::vector<u32>& pixels, f64 target) {
    f64 low = 0.0;
    f64 high = 8.0;
    for (int step = 0; step < 16; ++step) {
        const f64 middle = (low + high) / 2.0;
        (coverage(pixels, middle) < target ? low : high) = middle;
    }
    for (u32& pixel : pixels) {
        const auto alpha = static_cast<u32>(std::min(255.0, static_cast<f64>(channel(pixel, 3)) * high));
        pixel = (pixel & 0x00FFFFFFU) | (alpha << 24U);
    }
}

// The first pack of the stack with a usable `png`, decoded. Problems
// (missing, unreadable, frames that are not square) go to `warnings`; without
// any usable image the result is empty and the caller uses the checker.
struct Found {
    const PackSource* source = nullptr;
    std::optional<Decoded> image;
};

Found findImage(std::span<const PackSource* const> stack, const std::string& png,
                std::vector<std::string>& warnings) {
    // A broken image in a higher pack falls through to the next pack that
    // has a usable one, with a warning; the checker is for keys no pack can
    // provide.
    bool anyFile = false;
    for (const PackSource* pack : stack) {
        const std::optional<std::vector<std::byte>> bytes = pack->read(png);
        if (!bytes) {
            continue;
        }
        anyFile = true;
        std::optional<Decoded> image = decodeImage(*bytes);
        if (!image) {
            warnings.push_back(
                std::format("{} in pack '{}' is not a readable PNG", png, pack->manifest().id));
            continue;
        }
        if (image->height % image->width != 0) {
            warnings.push_back(std::format("{} in pack '{}' is {} x {}: frames must be square", png,
                                           pack->manifest().id, image->width, image->height));
            continue;
        }
        return {.source = pack, .image = std::move(image)};
    }
    if (!anyFile) {
        warnings.push_back(std::format("No pack has {}", png));
    }
    return {};
}

// Game ticks per frame from blocks/<key>.anim.toml ("frametime"), 1 without it.
u32 frameTicks(const PackSource& source, const std::string& key, std::vector<std::string>& warnings) {
    const auto anim = source.read(std::format("{}.anim.toml", textureFile(key)));
    if (!anim) {
        return 1;
    }
    try {
        const toml::table table = toml::parse(asText(*anim));
        return static_cast<u32>(std::clamp<i64>(table["frametime"].value_or(1), 1, 255));
    } catch (const toml::parse_error& error) {
        warnings.push_back(std::format("blocks/{}.anim.toml in pack '{}': {}", key, source.manifest().id,
                                       error.description()));
        return 1;
    }
}

} // namespace

Result<PackManifest> parsePackManifest(std::string_view text, std::string id) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& error) {
        return Error(ErrorCode::ParseError, std::format("pack.toml of '{}': {}", id, error.description()));
    }
    PackManifest manifest;
    manifest.id = std::move(id);
    const std::optional<std::string> name = root["name"].value<std::string>();
    const std::optional<i64> format = root["format"].value<i64>();
    const std::optional<i64> resolution = root["resolution"].value<i64>();
    if (!name || !format || !resolution) {
        return Error(ErrorCode::ParseError,
                     std::format("pack.toml of '{}' needs name, format and resolution", manifest.id));
    }
    if (*format < 1 || std::cmp_greater(*format, kPackFormat)) {
        return Error(ErrorCode::VersionMismatch,
                     std::format("pack '{}' has format {}; this version reads up to {}", manifest.id, *format,
                                 kPackFormat));
    }
    if (std::ranges::find(kPackResolutions, static_cast<u32>(*resolution)) == kPackResolutions.end()) {
        return Error(
            ErrorCode::InvalidArgument,
            std::format("pack '{}' has resolution {}; use 16, 32, 64, 128 or 256", manifest.id, *resolution));
    }
    manifest.name = *name;
    manifest.author = root["author"].value_or(std::string());
    manifest.version = root["version"].value_or(std::string());
    manifest.description = root["description"].value_or(std::string());
    manifest.format = static_cast<u32>(*format);
    manifest.resolution = static_cast<u32>(*resolution);
    manifest.mipmapAlphaPreserve = root["options"]["mipmap_alpha_preserve"].value_or(true);
    return manifest;
}

Result<std::unique_ptr<PackSource>> openPackFolder(const std::filesystem::path& folder) {
    auto text = io::readFile(folder / "pack.toml");
    if (!text) {
        return Error(ErrorCode::NotFound, std::format("{} has no pack.toml", folder.string()));
    }
    auto manifest = parsePackManifest(asText(text.value()), folder.filename().string());
    if (!manifest) {
        return manifest.error();
    }
    return std::unique_ptr<PackSource>(std::make_unique<FolderPack>(folder, std::move(manifest.value())));
}

Result<std::unique_ptr<PackSource>> openEmbeddedPack(std::string id, std::span<const EmbeddedFile> files) {
    const auto manifestFile = std::ranges::find(files, std::string_view("pack.toml"), &EmbeddedFile::path);
    if (manifestFile == files.end()) {
        return Error(ErrorCode::NotFound, std::format("built in pack '{}' has no pack.toml", id));
    }
    auto manifest = parsePackManifest(asText(std::as_bytes(manifestFile->bytes)), std::move(id));
    if (!manifest) {
        return manifest.error();
    }
    return std::unique_ptr<PackSource>(std::make_unique<EmbeddedPack>(std::move(manifest.value()), files));
}

std::vector<std::unique_ptr<PackSource>> discoverPacks(const std::filesystem::path& root,
                                                       std::vector<std::string>& problems) {
    std::vector<std::unique_ptr<PackSource>> packs;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (!entry.is_directory(error)) {
            continue;
        }
        auto pack = openPackFolder(entry.path());
        if (pack) {
            packs.push_back(std::move(pack.value()));
        } else {
            problems.push_back(pack.error().message());
        }
    }
    std::ranges::sort(packs, {}, [](const std::unique_ptr<PackSource>& pack) { return pack->manifest().id; });
    return packs;
}

namespace {

// The UI font of a pack, when it has one: a square sheet of 16 x 16 cells.
void validateFont(const std::filesystem::path& folder, std::vector<PackIssue>& issues) {
    const auto bytes = io::readFile(folder / "ui" / "font.png");
    if (!bytes) {
        return;
    }
    const std::optional<Decoded> font = decodeImage(bytes.value());
    if (!font) {
        issues.push_back({.error = true, .file = "ui/font.png", .message = "not a readable PNG"});
    } else if (font->width != font->height || font->width % 16 != 0) {
        issues.push_back(
            {.error = true,
             .file = "ui/font.png",
             .message = std::format(
                 "{} x {}: the font is a square sheet of 16 x 16 cells (128 x 128 for 8 pixel cells)",
                 font->width, font->height)});
    }
}

} // namespace

std::optional<PackImage> readPackImage(std::span<const PackSource* const> stack, std::string_view path,
                                       std::vector<std::string>& warnings) {
    for (const PackSource* pack : stack) {
        const std::optional<std::vector<std::byte>> bytes = pack->read(path);
        if (!bytes) {
            continue;
        }
        std::optional<Decoded> image = decodeImage(*bytes);
        if (!image) {
            warnings.push_back(
                std::format("{} in pack '{}' is not a readable image", path, pack->manifest().id));
            continue;
        }
        return PackImage{.path = std::string(path),
                         .source = pack->manifest().id,
                         .width = image->width,
                         .height = image->height,
                         .pixels = std::move(image->pixels)};
    }
    return std::nullopt;
}

std::vector<PackIssue> validatePack(const std::filesystem::path& folder,
                                    std::span<const std::string> referenceKeys) {
    std::vector<PackIssue> issues;
    const auto issue = [&](bool error, std::string file, std::string message) {
        issues.push_back({.error = error, .file = std::move(file), .message = std::move(message)});
    };
    auto manifestText = io::readFile(folder / "pack.toml");
    if (!manifestText) {
        issue(true, "pack.toml", "missing: every pack needs a pack.toml in its root folder");
        return issues;
    }
    const auto manifest = parsePackManifest(asText(manifestText.value()), folder.filename().string());
    if (!manifest) {
        issue(true, "pack.toml", manifest.error().message());
        return issues;
    }
    const u32 resolution = manifest.value().resolution;
    std::error_code error;
    if (!std::filesystem::is_directory(folder / "blocks", error)) {
        issue(false, "blocks", "no blocks/ folder: the pack changes no block textures");
        return issues;
    }
    std::vector<std::string> keys;
    std::vector<std::string> animations;
    for (const auto& entry : std::filesystem::directory_iterator(folder / "blocks", error)) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".anim.toml")) {
            animations.push_back(name.substr(0, name.size() - std::string_view(".anim.toml").size()));
        } else if (name.ends_with(".png")) {
            keys.push_back(name.substr(0, name.size() - 4));
        } else if (entry.is_regular_file(error)) {
            issue(false, "blocks/" + name, "not a .png or .anim.toml file; the loader ignores it");
        }
    }
    std::ranges::sort(keys);
    for (const std::string& key : keys) {
        const std::string file = "blocks/" + key + ".png";
        const auto bytes = io::readFile(folder / file);
        const std::optional<Decoded> image = bytes ? decodeImage(bytes.value()) : std::nullopt;
        if (!image) {
            issue(true, file, "not a readable PNG");
            continue;
        }
        if (!std::has_single_bit(image->width)) {
            issue(true, file,
                  std::format("{} pixels wide: use a power of two (16, 32, 64, 128 or 256)", image->width));
        } else if (image->height % image->width != 0) {
            issue(true, file,
                  std::format("{} x {}: textures are square, or a vertical strip of square frames",
                              image->width, image->height));
        } else if (image->width != resolution) {
            issue(false, file,
                  std::format("{} pixels wide but the pack says resolution {}; it will be scaled",
                              image->width, resolution));
        }
        if (image->height > image->width && std::ranges::find(animations, key) == animations.end()) {
            issue(false, file, "a frame strip without " + key + ".anim.toml: plays at 1 tick per frame");
        }
        if (!referenceKeys.empty() && std::ranges::find(referenceKeys, key) == referenceKeys.end()) {
            issue(false, file, "no block uses this texture key");
        }
    }
    for (const std::string& key : animations) {
        const std::string file = "blocks/" + key + ".anim.toml";
        if (!std::ranges::binary_search(keys, key)) {
            issue(false, file, "no " + key + ".png next to it");
        }
        const auto text = io::readFile(folder / file);
        try {
            const toml::table table = toml::parse(text ? asText(text.value()) : std::string_view());
            const i64 frametime = table["frametime"].value_or(i64{1});
            if (frametime < 1 || frametime > 255) {
                issue(false, file,
                      std::format("frametime {} is outside 1 to 255; it will be clamped", frametime));
            }
        } catch (const toml::parse_error& parseError) {
            issue(true, file, std::string(parseError.description()));
        }
    }
    validateFont(folder, issues);
    for (const std::string& key : referenceKeys) {
        if (!std::ranges::binary_search(keys, key)) {
            issue(false, "blocks/" + key + ".png", "missing: lower packs (or the default pack) provide it");
        }
    }
    return issues;
}

BlockTextureData buildBlockTextures(std::span<const PackSource* const> stack,
                                    std::span<const std::string> keys, const BlockTextureOptions& options) {
    VXE_PROFILE_FUNCTION();
    BlockTextureData data;
    u32 resolution = 16;
    for (const PackSource* pack : stack) {
        resolution = std::max(resolution, pack->manifest().resolution);
    }
    data.resolution =
        std::clamp(std::bit_floor(std::min(resolution, std::max(options.maxResolution, 16U))), 16U, 256U);
    data.mips = static_cast<u32>(std::countr_zero(data.resolution)) + 1;

    // Level 0 of every layer first: key layers, then extra animation frames.
    struct Layer {
        std::vector<u32> pixels;
        bool cutout = false;
        bool preserve = true;
    };
    std::vector<Layer> layers(keys.size());
    std::vector<Layer> extra;
    data.animations.assign(keys.size(), 0);
    data.sources.assign(keys.size(), std::string());
    for (usize k = 0; k < keys.size(); ++k) {
        const auto [source, image] =
            findImage(stack, std::format("{}.png", textureFile(keys[k])), data.warnings);
        if (!image) {
            layers[k].pixels = checker(data.resolution);
            continue;
        }
        data.sources[k] = source->manifest().id;
        layers[k].pixels = frameAt(*image, 0, data.resolution);
        layers[k].cutout = isCutout(layers[k].pixels);
        layers[k].preserve = source->manifest().mipmapAlphaPreserve;
        const u32 frames = image->height / image->width;
        if (frames > 1) {
            const u32 ticks = frameTicks(*source, keys[k], data.warnings);
            const auto firstExtra = static_cast<u32>(keys.size() + extra.size());
            for (u32 f = 1; f < frames; ++f) {
                extra.push_back({.pixels = frameAt(*image, f, data.resolution),
                                 .cutout = layers[k].cutout,
                                 .preserve = layers[k].preserve});
            }
            data.animations[k] = std::min(frames, 255U) | (ticks << 8U) | (firstExtra << 16U);
        }
    }
    layers.insert(layers.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
    data.layers = static_cast<u32>(layers.size());

    // The mip chain of each layer.
    data.pixels.resize(data.mips);
    for (u32 mip = 0; mip < data.mips; ++mip) {
        const usize side = data.resolution >> mip;
        data.pixels[mip].reserve(side * side * layers.size());
    }
    data.averageColors.assign(keys.size(), 0);
    for (usize l = 0; l < layers.size(); ++l) {
        std::vector<u32> level = std::move(layers[l].pixels);
        const f64 target = layers[l].cutout && layers[l].preserve ? coverage(level, 1.0) : -1.0;
        u32 side = data.resolution;
        for (u32 mip = 0; mip < data.mips; ++mip) {
            data.pixels[mip].insert(data.pixels[mip].end(), level.begin(), level.end());
            if (mip + 1 == data.mips) {
                if (l < keys.size()) {
                    data.averageColors[l] = level.front();
                }
                break;
            }
            level = halve(level, side);
            side /= 2;
            if (target >= 0.0) {
                preserveCoverage(level, target);
            }
        }
    }
    return data;
}

} // namespace vxe::render
