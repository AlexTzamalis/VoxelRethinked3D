#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Texture packs (Section 15): folders of PNG files that give block textures
// their look. This file is the CPU side: reading pack manifests, finding
// packs, and turning a stack of packs plus the game's texture keys into the
// pixels of a texture array (decoded, flipped for the RHI, scaled to one
// size, mipmapped, animations expanded). It never touches the GPU, so it runs
// on a job; TexturePackManager uploads the result.
namespace vxe::render {

// The newest pack format this engine reads (pack.toml "format").
inline constexpr u32 kPackFormat = 1;
// Block texture sizes a pack may declare (Section 15.2).
inline constexpr std::array<u32, 5> kPackResolutions = {16, 32, 64, 128, 256};

struct PackManifest {
    std::string id; // The folder name; how settings name the pack.
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    u32 format = kPackFormat;
    u32 resolution = 16;
    bool mipmapAlphaPreserve = true;
};

// Reads a pack.toml. `id` becomes the manifest's id. Fails with ParseError
// on malformed text or missing fields, VersionMismatch on a newer format,
// InvalidArgument on a resolution that is not 16, 32, 64, 128 or 256.
[[nodiscard]] Result<PackManifest> parsePackManifest(std::string_view text, std::string id);

// Where a pack's files come from: a folder on disk, or memory (the default
// pack built into the executable as the last fallback, Section 15.1).
// read() takes paths inside the pack ("pack.toml", "blocks/stone.png") and
// returns nothing for missing files. Thread safety: read() may be called
// from any thread.
class PackSource {
public:
    PackSource() = default;
    virtual ~PackSource() = default;
    PackSource(const PackSource&) = delete;
    PackSource& operator=(const PackSource&) = delete;
    PackSource(PackSource&&) = delete;
    PackSource& operator=(PackSource&&) = delete;

    [[nodiscard]] virtual const PackManifest& manifest() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> read(std::string_view path) const = 0;
};

// A pack in a folder. Fails like parsePackManifest, or with NotFound when the
// folder has no pack.toml.
[[nodiscard]] Result<std::unique_ptr<PackSource>> openPackFolder(const std::filesystem::path& folder);

// One embedded file of a built in pack.
struct EmbeddedFile {
    std::string_view path;
    std::span<const unsigned char> bytes;
};
// A pack from files built into the executable.
[[nodiscard]] Result<std::unique_ptr<PackSource>> openEmbeddedPack(std::string id,
                                                                   std::span<const EmbeddedFile> files);

// Every pack folder under `root` (folders with a pack.toml), sorted by id.
// Folders that fail to open are reported in `problems` and left out.
[[nodiscard]] std::vector<std::unique_ptr<PackSource>> discoverPacks(const std::filesystem::path& root,
                                                                     std::vector<std::string>& problems);

// The pixels of a block texture array, ready to upload. Layer i of the first
// keys.size() layers is key i (the game's layer numbers, which stay the same
// whatever packs are on); frames 2 and later of animated textures follow.
// Rows are bottom first (ADR-016). Every layer has `mips` levels, level 0 at
// `resolution`.
struct BlockTextureData {
    u32 resolution = 16;
    u32 layers = 0;
    u32 mips = 1;
    // pixels[mip] holds every layer of that level, layer after layer, RGBA8.
    std::vector<std::vector<u32>> pixels;
    // Per key layer: 0 for a still texture, else frame count in bits 0..7,
    // game ticks (1/20 s) per frame in bits 8..15 and the layer of frame 2
    // in bits 16..31 (render/chunk.vert reads it).
    std::vector<u32> animations;
    // Per key layer: the average color (its smallest mip), for the far
    // terrain color table (Section 13.2).
    std::vector<u32> averageColors;
    // Per key: the id of the pack that provided it, empty for the missing
    // texture checker.
    std::vector<std::string> sources;
    // Every problem found (missing textures, broken images); never fatal.
    std::vector<std::string> warnings;
};

struct BlockTextureOptions {
    u32 maxResolution = 256; // Graphics.Textures.MaxResolution.
};

// Builds the array from a stack of packs, highest priority first: each key
// comes from the first pack that has blocks/<key>.png. The array resolution
// is the largest resolution among the packs (capped by maxResolution); other
// textures are scaled with nearest filtering, which keeps pixel art sharp. A
// key no pack has, or whose image is broken, gets a magenta and black
// checker and a warning. A blocks/<key>.anim.toml next to a texture makes it
// animated: the image is a vertical strip of square frames, and
// "frametime" sets game ticks per frame (default 1). Mipmaps average 2 x 2
// texels; for textures with cut out pixels (leaves), alpha is scaled per
// level so the share of visible pixels stays the same and leaves do not
// thin out with distance (pack option mipmap_alpha_preserve).
//
// Thread safety: reads the sources only; run it on a job.
[[nodiscard]] BlockTextureData buildBlockTextures(std::span<const PackSource* const> stack,
                                                  std::span<const std::string> keys,
                                                  const BlockTextureOptions& options = {});

} // namespace vxe::render
