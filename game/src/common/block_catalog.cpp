#include "block_catalog.hpp"

#include <vxe/core/error.hpp>
#include <vxe/io/file_io.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <map>
#include <system_error>
#include <utility>

namespace vr {

using vxe::Error;
using vxe::ErrorCode;
using vxe::Result;
using vxe::u16;
using vxe::usize;
using vxe::voxel::BlockFace;
using vxe::voxel::BlockRenderInfo;
using vxe::voxel::BlockStateId;
using vxe::voxel::RenderLayer;

namespace {

constexpr usize kMaxTextureLayers = 16384; // 14 bits in the packed quad (Section 10.4).
constexpr usize kMaxReportedProblems = 50;

// Lowercase letters, digits and underscores: names end up in save files and
// texture paths, so they must be portable and unambiguous.
bool isValidName(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// Texture keys for the six faces (BlockFace order), filled in from the most
// general field to the most specific so specific ones win.
using FaceKeys = std::array<std::string, vxe::voxel::kBlockFaceCount>;

constexpr std::array<std::pair<std::string_view, BlockFace>, 6> kFaceFields = {{
    {"east", BlockFace::PosX},
    {"west", BlockFace::NegX},
    {"up", BlockFace::PosY},
    {"down", BlockFace::NegY},
    {"south", BlockFace::PosZ},
    {"north", BlockFace::NegZ},
}};

} // namespace

// Reads the files into a catalog, collecting every problem found.
class BlockCatalogLoader {
public:
    Result<BlockCatalog> run(std::span<const BlockSource> sources);

private:
    void problem(std::string_view file, std::string message) {
        m_problems.push_back(std::format("{}: {}", file, std::move(message)));
    }
    void loadFile(const BlockSource& source);
    bool readCommon(const BlockSource& source, const toml::table& root, BlockRenderInfo& info);
    bool applyTextures(std::string_view file, std::string_view where, const toml::node* node, FaceKeys& keys);
    void addState(std::string_view file, std::string name, const BlockRenderInfo& base, const FaceKeys& keys);
    // One state per [[variants]] table; returns how many were added.
    u16 addVariants(std::string_view file, const std::string& name, const toml::array& variants,
                    const BlockRenderInfo& info, const FaceKeys& baseKeys);
    u16 layerOf(const std::string& key);

    BlockCatalog m_catalog;
    std::vector<std::string> m_problems;
    vxe::StringMap<u16> m_layers;
};

Result<BlockCatalog> BlockCatalogLoader::run(std::span<const BlockSource> sources) {
    m_catalog.m_stateNames.emplace_back("air");
    m_catalog.m_renderInfos.push_back(vxe::voxel::airRenderInfo());
    m_catalog.m_stateIds.emplace("air", vxe::voxel::kAirState);

    // File name order, so the same files always give the same state ids.
    std::vector<const BlockSource*> ordered;
    ordered.reserve(sources.size());
    for (const BlockSource& source : sources) {
        ordered.push_back(&source);
    }
    std::ranges::sort(ordered, [](const BlockSource* a, const BlockSource* b) { return a->file < b->file; });
    for (const BlockSource* source : ordered) {
        loadFile(*source);
    }
    if (m_catalog.m_blocks.empty() && m_problems.empty()) {
        m_problems.emplace_back("no block definitions found");
    }
    if (!m_problems.empty()) {
        std::string message = std::format("{} problem{} in the block definitions:", m_problems.size(),
                                          m_problems.size() == 1 ? "" : "s");
        for (usize i = 0; i < std::min(m_problems.size(), kMaxReportedProblems); ++i) {
            message += "\n  " + m_problems[i];
        }
        return Error(ErrorCode::ParseError, std::move(message));
    }
    return std::move(m_catalog);
}

void BlockCatalogLoader::loadFile(const BlockSource& source) {
    toml::table root;
    // toml++ reports syntax errors by exception; the game does not throw
    // (Section 23.4), so it becomes one problem here.
    try {
        root = toml::parse(source.text);
    } catch (const toml::parse_error& error) {
        const auto& where = error.source().begin;
        problem(source.file, std::format("not valid TOML (line {}, column {}): {}", where.line, where.column,
                                         error.description()));
        return;
    }
    for (const auto& [key, node] : root) {
        static constexpr std::array<std::string_view, 9> kKnown = {
            "name", "display_name", "layer",   "emitted_light", "light_opacity", "culls_same_type",
            "tint", "textures",     "variants"};
        if (std::ranges::find(kKnown, key.str()) == kKnown.end()) {
            problem(source.file, std::format("unknown field '{}'", key.str()));
        }
    }

    const std::optional<std::string> name = root["name"].value<std::string>();
    const std::optional<std::string> displayName = root["display_name"].value<std::string>();
    if (!name || !isValidName(*name)) {
        problem(source.file, "'name' must be lowercase letters, digits and underscores (at most 64)");
        return;
    }
    if (!displayName || displayName->empty()) {
        problem(source.file, "'display_name' is missing");
    }
    if (m_catalog.m_blockIndex.contains(*name)) {
        problem(source.file, std::format("block '{}' is defined twice", *name));
        return;
    }
    BlockRenderInfo info;
    if (!readCommon(source, root, info)) {
        return;
    }
    FaceKeys baseKeys;
    if (!applyTextures(source.file, "textures", root.get("textures"), baseKeys)) {
        return;
    }

    BlockDefinition block{.name = *name,
                          .displayName = displayName.value_or(*name),
                          .file = source.file,
                          .firstState = static_cast<BlockStateId>(m_catalog.m_stateNames.size()),
                          .stateCount = 0};
    const toml::node* variantsNode = root.get("variants");
    if (variantsNode == nullptr) {
        addState(source.file, *name, info, baseKeys);
        block.stateCount = 1;
    } else {
        const toml::array* variants = variantsNode->as_array();
        if (variants == nullptr || variants->empty()) {
            problem(source.file, "'variants' must be a non empty list of [[variants]] tables");
            return;
        }
        block.stateCount = addVariants(source.file, *name, *variants, info, baseKeys);
    }
    m_catalog.m_blockIndex.emplace(*name, m_catalog.m_blocks.size());
    m_catalog.m_blocks.push_back(std::move(block));
}

u16 BlockCatalogLoader::addVariants(std::string_view file, const std::string& name,
                                    const toml::array& variants, const BlockRenderInfo& info,
                                    const FaceKeys& baseKeys) {
    u16 added = 0;
    for (usize index = 0; index < variants.size(); ++index) {
        const std::string where = std::format("variants[{}]", index);
        const toml::table* variant = variants.get(index)->as_table();
        const toml::table* properties =
            variant != nullptr ? variant->get_as<toml::table>("properties") : nullptr;
        if (properties == nullptr || properties->empty()) {
            problem(file, where + " needs a non empty 'properties' table");
            continue;
        }
        // Keys sorted (std::map) so the state name never depends on the
        // order they were written in.
        std::map<std::string, std::string, std::less<>> sorted;
        for (const auto& [key, value] : *properties) {
            const std::optional<std::string> text = value.value<std::string>();
            if (!isValidName(key.str()) || !text || !isValidName(*text)) {
                problem(file, std::format("{}.properties.{} must be a lowercase word", where, key.str()));
                continue;
            }
            sorted.emplace(std::string(key.str()), *text);
        }
        std::string stateName = name + "[";
        for (const auto& [key, value] : sorted) {
            stateName += std::format("{}{}={}", stateName.back() == '[' ? "" : ",", key, value);
        }
        stateName += "]";
        FaceKeys keys = baseKeys;
        if (!applyTextures(file, where + ".textures", variant->get("textures"), keys)) {
            continue;
        }
        addState(file, stateName, info, keys);
        ++added;
    }
    return added;
}

bool BlockCatalogLoader::readCommon(const BlockSource& source, const toml::table& root,
                                    BlockRenderInfo& info) {
    const std::optional<std::string> layer = root["layer"].value<std::string>();
    if (layer == "opaque") {
        info.layer = RenderLayer::Opaque;
    } else if (layer == "cutout") {
        info.layer = RenderLayer::Cutout;
    } else if (layer == "translucent") {
        info.layer = RenderLayer::Translucent;
    } else if (layer == "invisible") {
        info.layer = RenderLayer::Invisible;
    } else {
        problem(source.file,
                std::format("'layer' must be opaque, cutout, translucent or invisible (got '{}')",
                            layer.value_or("nothing")));
        return false;
    }
    bool valid = true;
    const auto integer = [&](std::string_view key, std::int64_t fallback, std::int64_t max) -> vxe::u8 {
        const auto node = root[key];
        if (!node) {
            return static_cast<vxe::u8>(fallback);
        }
        const std::optional<std::int64_t> value = node.value_exact<std::int64_t>();
        if (!value || *value < 0 || *value > max) {
            problem(source.file, std::format("'{}' must be a whole number from 0 to {}", key, max));
            valid = false;
            return 0;
        }
        return static_cast<vxe::u8>(*value);
    };
    info.emittedLight = integer("emitted_light", 0, vxe::voxel::kMaxLightLevel);
    // Opaque blocks stop light by default; everything else lets it through.
    info.lightOpacity =
        integer("light_opacity", info.layer == RenderLayer::Opaque ? vxe::voxel::kMaxLightLevel : 0,
                vxe::voxel::kMaxLightLevel);
    info.tintIndex = integer("tint", 0, 255);
    if (const auto culls = root["culls_same_type"]; culls) {
        if (!culls.is_boolean()) {
            problem(source.file, "'culls_same_type' must be true or false");
            valid = false;
        } else {
            info.cullsSameType = culls.value_or(true);
        }
    }
    return valid;
}

bool BlockCatalogLoader::applyTextures(std::string_view file, std::string_view where, const toml::node* node,
                                       FaceKeys& keys) {
    if (node == nullptr) {
        return true;
    }
    const toml::table* table = node->as_table();
    if (table == nullptr) {
        problem(file, std::format("'{}' must be a table of texture keys", where));
        return false;
    }
    bool valid = true;
    for (const auto& [key, value] : *table) {
        const std::optional<std::string> texture = value.value<std::string>();
        const std::string_view field = key.str();
        const bool known =
            field == "all" || field == "side" || field == "top" || field == "bottom" ||
            std::ranges::any_of(kFaceFields, [&](const auto& entry) { return entry.first == field; });
        if (!known) {
            problem(file,
                    std::format("{}: unknown face '{}' (use all, side, top, bottom, east, west, up, down, "
                                "south or north)",
                                where, key.str()));
            valid = false;
        } else if (!texture || !isValidName(*texture)) {
            problem(file, std::format("{}.{} must be a texture key like \"stone\"", where, key.str()));
            valid = false;
        }
    }
    if (!valid) {
        return false;
    }
    const auto set = [&](std::string_view field, std::initializer_list<BlockFace> faces) {
        if (const std::optional<std::string> texture = (*table)[field].value<std::string>()) {
            for (const BlockFace face : faces) {
                keys.at(static_cast<usize>(face)) = *texture;
            }
        }
    };
    set("all", {BlockFace::PosX, BlockFace::NegX, BlockFace::PosY, BlockFace::NegY, BlockFace::PosZ,
                BlockFace::NegZ});
    set("side", {BlockFace::PosX, BlockFace::NegX, BlockFace::PosZ, BlockFace::NegZ});
    set("top", {BlockFace::PosY});
    set("bottom", {BlockFace::NegY});
    for (const auto& [field, face] : kFaceFields) {
        set(field, {face});
    }
    return true;
}

void BlockCatalogLoader::addState(std::string_view file, std::string name, const BlockRenderInfo& base,
                                  const FaceKeys& keys) {
    if (m_catalog.m_stateIds.contains(name)) {
        problem(file, std::format("state '{}' is defined twice", name));
        return;
    }
    if (m_catalog.m_stateNames.size() >= vxe::voxel::kMaxBlockStates) {
        problem(file, "more than 65,536 block states");
        return;
    }
    BlockRenderInfo info = base;
    if (info.layer != RenderLayer::Invisible) {
        for (usize face = 0; face < keys.size(); ++face) {
            if (keys.at(face).empty()) {
                problem(file, std::format("state '{}' has no texture for its {} face", name,
                                          kFaceFields.at(face).first));
                return;
            }
            info.faceTextureLayer.at(face) = layerOf(keys.at(face));
        }
    }
    const auto id = static_cast<BlockStateId>(m_catalog.m_stateNames.size());
    m_catalog.m_stateIds.emplace(name, id);
    m_catalog.m_stateNames.push_back(std::move(name));
    m_catalog.m_renderInfos.push_back(info);
}

u16 BlockCatalogLoader::layerOf(const std::string& key) {
    if (const auto it = m_layers.find(key); it != m_layers.end()) {
        return it->second;
    }
    if (m_catalog.m_textureKeys.size() >= kMaxTextureLayers) {
        problem("textures", "more than 16,384 distinct texture keys");
        return 0;
    }
    const auto layer = static_cast<u16>(m_catalog.m_textureKeys.size());
    m_catalog.m_textureKeys.push_back(key);
    m_layers.emplace(key, layer);
    return layer;
}

Result<BlockCatalog> BlockCatalog::load(std::span<const BlockSource> sources) {
    return BlockCatalogLoader().run(sources);
}

Result<BlockCatalog> BlockCatalog::loadFolder(const std::filesystem::path& folder) {
    std::error_code error;
    const std::filesystem::directory_iterator it(folder, error);
    if (error) {
        return Error(ErrorCode::NotFound, std::format("Block definitions folder '{}' cannot be read: {}",
                                                      folder.string(), error.message()));
    }
    std::vector<BlockSource> sources;
    for (const std::filesystem::directory_entry& entry : it) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".toml") {
            continue;
        }
        Result<std::vector<std::byte>> bytes = vxe::io::readFile(entry.path());
        if (!bytes) {
            return std::move(bytes).error();
        }
        const std::vector<std::byte>& data = bytes.value();
        sources.push_back({.file = entry.path().filename().string(),
                           .text = std::string(reinterpret_cast<const char*>(data.data()), data.size())});
    }
    return load(sources);
}

const BlockDefinition* BlockCatalog::block(std::string_view name) const {
    const auto it = m_blockIndex.find(name);
    return it == m_blockIndex.end() ? nullptr : &m_blocks.at(it->second);
}

std::optional<BlockStateId> BlockCatalog::stateId(std::string_view stateName) const {
    const auto it = m_stateIds.find(stateName);
    return it == m_stateIds.end() ? std::nullopt : std::optional(it->second);
}

std::string_view BlockCatalog::stateName(BlockStateId state) const {
    return state < m_stateNames.size() ? std::string_view(m_stateNames[state]) : std::string_view("air");
}

std::optional<BlockStateId> BlockCatalog::defaultState(std::string_view blockName) const {
    const BlockDefinition* definition = block(blockName);
    return definition == nullptr ? std::nullopt : std::optional(definition->firstState);
}

Result<std::shared_ptr<vxe::voxel::BlockRegistryView>> BlockCatalog::buildRegistry() const {
    vxe::voxel::BlockRegistryBuilder builder;
    // State 0 (air) is in every builder already; the rest go in id order.
    for (usize state = 1; state < m_renderInfos.size(); ++state) {
        Result<BlockStateId> id = builder.add(m_renderInfos[state]);
        if (!id) {
            return std::move(id).error();
        }
        VXE_ASSERT(id.value() == state, "Block state ids must match the catalog");
    }
    return builder.build();
}

} // namespace vr
