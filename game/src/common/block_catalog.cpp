#include "block_catalog.hpp"

#include <vxe/core/error.hpp>
#include <vxe/io/file_io.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
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
using vxe::u32;
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
bool isValidName(std::string_view name);

// A texture key is a name, with one folder in front when it lives outside
// blocks/, like "foliage/poppy".
bool isTextureKey(std::string_view key) {
    const auto slash = key.find('/');
    return slash == std::string_view::npos
               ? isValidName(key)
               : isValidName(key.substr(0, slash)) && isValidName(key.substr(slash + 1));
}

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
    bool readTintFaces(const BlockSource& source, const toml::table& root);
    // Reads `variations`, `variation_overlays` and `bushy`.
    bool readVariations(const BlockSource& source, const toml::table& root, BlockRenderInfo& info);
    void linkTallPlants();
    void setLook(std::string_view file, u16 layer, u32 look);
    u16 layerOf(const std::string& key);

    BlockCatalog m_catalog;
    std::vector<std::string> m_problems;
    vxe::StringMap<u16> m_layers;
    std::vector<bool> m_lookSet;                                 // Per layer: a block has chosen its look.
    FaceKeys m_overlays;                                         // Of the block being read.
    std::array<bool, 6> m_tintFaces{};                           // Of the block being read.
    vxe::u8 m_flags = 0;                                         // Of the block being read.
    bool m_hasVariations = false;                                // Of the block being read.
    bool m_overlayTinted = true;                                 // Of the block being read.
    std::optional<BlockCatalog::AimBox> m_aimBox;                // Of the block being read, from aim_box.
    std::vector<std::pair<std::string, std::string>> m_tallTops; // Lower and upper block names.
};

Result<BlockCatalog> BlockCatalogLoader::run(std::span<const BlockSource> sources) {
    m_catalog.m_tintColors.fill(0xFFFFFFFFU);
    m_catalog.m_stateNames.emplace_back("air");
    m_catalog.m_stateFlags.push_back(0);
    m_catalog.m_renderInfos.push_back(vxe::voxel::airRenderInfo());
    m_catalog.m_aimBoxes.push_back({0, 0, 0, 16, 16, 16});
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
    linkTallPlants();
    // Waterlogged blocks stand in water: the cell is water around them.
    const auto water = m_catalog.m_stateIds.find("water");
    for (usize state = 0; state < m_catalog.m_stateFlags.size(); ++state) {
        if ((m_catalog.m_stateFlags[state] & 16U) == 0) {
            continue;
        }
        if (water == m_catalog.m_stateIds.end()) {
            m_problems.push_back(
                std::format("{}: waterlogged needs a block named water", m_catalog.m_stateNames[state]));
            break;
        }
        vxe::voxel::BlockRenderInfo& info = m_catalog.m_renderInfos[state];
        info.fluid = water->second;
        info.lightOpacity =
            std::max(info.lightOpacity, m_catalog.m_renderInfos.at(water->second).lightOpacity);
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
        static constexpr std::array<std::string_view, 26> kKnown = {"name",
                                                                    "display_name",
                                                                    "layer",
                                                                    "emitted_light",
                                                                    "light_opacity",
                                                                    "culls_same_type",
                                                                    "tint",
                                                                    "tint_color",
                                                                    "tint_faces",
                                                                    "textures",
                                                                    "overlays",
                                                                    "variants",
                                                                    "solid",
                                                                    "liquid",
                                                                    "category",
                                                                    "variations",
                                                                    "variation_overlays",
                                                                    "bushy",
                                                                    "replaceable",
                                                                    "needs_support",
                                                                    "overlay_tint",
                                                                    "tall_top",
                                                                    "hidden",
                                                                    "height",
                                                                    "aim_box",
                                                                    "waterlogged"};
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
    m_overlays = {};
    if (!applyTextures(source.file, "overlays", root.get("overlays"), m_overlays) ||
        !readTintFaces(source, root) || !readVariations(source, root, info)) {
        return;
    }
    if (const std::optional<std::string> top = root["tall_top"].value<std::string>()) {
        m_tallTops.emplace_back(*name, *top);
    }

    BlockDefinition block{.name = *name,
                          .displayName = displayName.value_or(*name),
                          .file = source.file,
                          .category = root["category"].value_or(std::string("Building")),
                          .hidden = root["hidden"].value_or(false),
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
        BlockRenderInfo variantInfo = info;
        if (const toml::node* heightNode = variant->get("height"); heightNode != nullptr) {
            const std::optional<std::int64_t> height = heightNode->value_exact<std::int64_t>();
            if (!height || *height < 1 || *height > 16) {
                problem(file, where + ".height must be a whole number from 1 to 16");
                continue;
            }
            variantInfo.height = *height == 16 ? vxe::u8{0} : static_cast<vxe::u8>(*height);
        }
        addState(file, stateName, variantInfo, keys);
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
    } else if (layer == "plant") {
        info.layer = RenderLayer::Plant;
    } else if (layer == "invisible") {
        info.layer = RenderLayer::Invisible;
    } else {
        problem(source.file,
                std::format("'layer' must be opaque, cutout, translucent, plant or invisible (got '{}')",
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
    info.tintIndex = integer("tint", 0, vxe::voxel::kTintCount - 1);
    // Sixteenths of a block; 16, the default, is a whole block (stored as 0).
    const vxe::u8 height = integer("height", 16, 16);
    info.height = height == 0 || height == 16 ? 0 : height;
    const auto flag = [&](std::string_view key, bool fallback) {
        const auto node = root[key];
        if (node && !node.is_boolean()) {
            problem(source.file, std::format("'{}' must be true or false", key));
            valid = false;
        }
        return node.value_or(fallback);
    };
    const bool plant = info.layer == RenderLayer::Plant;
    const bool solid = flag("solid", info.layer != RenderLayer::Invisible && !plant);
    const bool liquid = flag("liquid", false);
    const bool replaceable = flag("replaceable", plant);
    const bool needsSupport = flag("needs_support", plant);
    const bool waterlogged = flag("waterlogged", false);
    m_flags = static_cast<vxe::u8>((solid ? 1U : 0U) | (liquid ? 2U : 0U) | (replaceable ? 4U : 0U) |
                                   (needsSupport ? 8U : 0U) | (waterlogged ? 16U : 0U));
    m_overlayTinted = flag("overlay_tint", true);
    m_aimBox.reset();
    if (const toml::node* boxNode = root.get("aim_box"); boxNode != nullptr) {
        const toml::array* box = boxNode->as_array();
        BlockCatalog::AimBox read{};
        bool good = box != nullptr && box->size() == read.size();
        for (usize i = 0; good && i < read.size(); ++i) {
            const std::optional<std::int64_t> value = box->get(i)->value_exact<std::int64_t>();
            good = value && *value >= 0 && *value <= 16;
            read.at(i) = good ? static_cast<vxe::u8>(*value) : vxe::u8{0};
        }
        good = good && read[0] < read[3] && read[1] < read[4] && read[2] < read[5];
        if (!good) {
            problem(source.file, "'aim_box' must be six whole numbers from 0 to 16: x0, y0, z0, x1, y1, z1, "
                                 "each start below its end");
            valid = false;
        } else {
            m_aimBox = read;
        }
    }
    if (const auto color = root["tint_color"]; color) {
        const std::string text = color.value_or(std::string());
        u32 value = 0;
        const char* end = text.data() + text.size();
        const bool isColor = text.size() == 7 && text.front() == '#' &&
                             std::from_chars(text.data() + 1, end, value, 16).ptr == end;
        if (!isColor || info.tintIndex == 0) {
            problem(source.file, "'tint_color' must look like \"#8DBB5A\", on a block with a tint");
            valid = false;
        } else {
            // From 0xRRGGBB to the 0xAABBGGRR of texture colors.
            const u32 tint = 0xFF000000U | ((value & 0xFFU) << 16U) | (value & 0xFF00U) | (value >> 16U);
            u32& stored = m_catalog.m_tintColors.at(info.tintIndex);
            if (stored != 0xFFFFFFFFU && stored != tint) {
                problem(source.file, std::format("tint {} has two different colors", info.tintIndex));
                valid = false;
            }
            stored = tint;
        }
    }
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

// tint_faces: which faces the block's tint colors. Default: all of them.
bool BlockCatalogLoader::readTintFaces(const BlockSource& source, const toml::table& root) {
    m_tintFaces.fill(true);
    const toml::node* node = root.get("tint_faces");
    if (node == nullptr) {
        return true;
    }
    const toml::array* faces = node->as_array();
    if (faces == nullptr) {
        problem(source.file, R"('tint_faces' must be a list like ["top", "side"])");
        return false;
    }
    m_tintFaces.fill(false);
    for (const toml::node& entry : *faces) {
        const std::string field = entry.value_or(std::string());
        if (field == "all") {
            m_tintFaces.fill(true);
        } else if (field == "top" || field == "bottom" || field == "side") {
            const auto mark = [this](BlockFace face) { m_tintFaces.at(static_cast<usize>(face)) = true; };
            if (field == "top") {
                mark(BlockFace::PosY);
            } else if (field == "bottom") {
                mark(BlockFace::NegY);
            } else {
                for (const BlockFace face :
                     {BlockFace::PosX, BlockFace::NegX, BlockFace::PosZ, BlockFace::NegZ}) {
                    mark(face);
                }
            }
        } else if (const auto known =
                       std::ranges::find(kFaceFields, field, [](const auto& pair) { return pair.first; });
                   known != kFaceFields.end()) {
            m_tintFaces.at(static_cast<usize>(known->second)) = true;
        } else {
            problem(source.file, std::format("tint_faces: unknown face '{}'", field));
            return false;
        }
    }
    return true;
}

// A texture has one look: two blocks may share it only if they color it alike.
void BlockCatalogLoader::setLook(std::string_view file, u16 layer, u32 look) {
    if (m_lookSet.at(layer) && m_catalog.m_textureLooks.at(layer) != look) {
        problem(file, std::format("texture '{}' is tinted or overlaid in two different ways",
                                  m_catalog.m_textureKeys.at(layer)));
    }
    m_lookSet.at(layer) = true;
    m_catalog.m_textureLooks.at(layer) = look;
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
        } else if (!texture || !isTextureKey(*texture)) {
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
            if (m_hasVariations) {
                continue;
            }
            if (keys.at(face).empty()) {
                problem(file, std::format("state '{}' has no texture for its {} face", name,
                                          kFaceFields.at(face).first));
                return;
            }
            info.faceTextureLayer.at(face) = layerOf(keys.at(face));
            const u32 tint = m_tintFaces.at(face) ? info.tintIndex : 0U;
            const std::string& overlay = m_overlays.at(face);
            if (m_hasVariations) {
                continue; // readVariations set the layer and its looks.
            }
            const u32 baseTint = m_overlayTinted ? 0U : tint;
            const u32 overlayTint = m_overlayTinted ? tint : 0U;
            setLook(file, info.faceTextureLayer.at(face),
                    overlay.empty()
                        ? vxe::voxel::textureLook(0, tint, 0)
                        : vxe::voxel::textureLook(u32{layerOf(overlay)} + 1U, baseTint, overlayTint));
        }
    }
    const auto id = static_cast<BlockStateId>(m_catalog.m_stateNames.size());
    m_catalog.m_stateIds.emplace(name, id);
    m_catalog.m_stateNames.push_back(std::move(name));
    m_catalog.m_renderInfos.push_back(info);
    m_catalog.m_stateFlags.push_back(m_flags);
    // Plants: a box like Minecraft's grass, 12 of 16 wide and 13 tall.
    BlockCatalog::AimBox box{0, 0, 0, 16, 16, 16};
    if (m_aimBox) {
        box = *m_aimBox;
    } else if (info.layer == RenderLayer::Plant) {
        box = {2, 0, 2, 14, 13, 14};
    } else if (info.height != 0) {
        box[4] = info.height;
    }
    m_catalog.m_aimBoxes.push_back(box);
}

// Variations sit in consecutive layers, so a mesher only adds a number to the
// first: they must be new keys, used by this block alone.
bool BlockCatalogLoader::readVariations(const BlockSource& source, const toml::table& root,
                                        BlockRenderInfo& info) {
    if (const std::optional<std::string> bushy = root["bushy"].value<std::string>()) {
        if (!isTextureKey(*bushy)) {
            problem(source.file, "'bushy' must be a texture key");
            return false;
        }
        const u16 layer = layerOf(*bushy);
        setLook(source.file, layer, vxe::voxel::textureLook(0, info.tintIndex, 0));
        info.bushyLayer = static_cast<u16>(layer + 1U);
    }
    const toml::array* list = root.get_as<toml::array>("variations");
    m_hasVariations = list != nullptr;
    if (list == nullptr) {
        return true;
    }
    if (list->empty() || list->size() > 255) {
        problem(source.file, "'variations' needs 1 to 255 texture keys");
        return false;
    }
    const auto first = static_cast<u16>(m_catalog.m_textureKeys.size());
    const u32 tint = info.tintIndex;
    for (const toml::node& entry : *list) {
        const std::string key = entry.value_or(std::string());
        if (!isTextureKey(key) || m_layers.contains(key)) {
            problem(source.file, std::format("variation '{}' must be a new texture key", key));
            return false;
        }
        setLook(source.file, layerOf(key), vxe::voxel::textureLook(0, tint, 0));
    }
    info.variationCount = static_cast<vxe::u8>(list->size());
    info.faceTextureLayer.fill(first);
    if (const toml::table* overlays = root.get_as<toml::table>("variation_overlays")) {
        for (const auto& [key, value] : *overlays) {
            const std::optional<std::string> overlay = value.value<std::string>();
            const auto base = m_layers.find(std::string(key.str()));
            if (!overlay || !isTextureKey(*overlay) || base == m_layers.end()) {
                problem(source.file,
                        std::format("variation_overlays: '{}' is not one of the variations", key.str()));
                return false;
            }
            const u16 baseLayer = base->second;
            // Seed heads and the like keep their own color over the tinted plant.
            const u16 overlayLayer = layerOf(*overlay);
            m_catalog.m_textureLooks.at(baseLayer) = vxe::voxel::textureLook(u32{overlayLayer} + 1U, tint, 0);
        }
    }
    return true;
}

vxe::voxel::BlockStateId BlockCatalog::nextLayer(vxe::voxel::BlockStateId state) const {
    const usize next = usize{state} + 1;
    if (state == vxe::voxel::kAirState || next >= m_renderInfos.size() ||
        m_renderInfos.at(state).height == 0 ||
        !sameBlock(state, static_cast<vxe::voxel::BlockStateId>(next))) {
        return vxe::voxel::kAirState;
    }
    const vxe::u8 taller = m_renderInfos.at(next).height;
    return taller == 0 || taller > m_renderInfos.at(state).height
               ? static_cast<vxe::voxel::BlockStateId>(next)
               : vxe::voxel::kAirState;
}

bool BlockCatalog::sameBlock(vxe::voxel::BlockStateId a, vxe::voxel::BlockStateId b) const {
    const auto blockName = [&](vxe::voxel::BlockStateId state) {
        const std::string_view name = m_stateNames.at(state);
        return name.substr(0, name.find('['));
    };
    return blockName(a) == blockName(b);
}

void BlockCatalogLoader::linkTallPlants() {
    m_catalog.m_tallUpper.assign(m_catalog.m_stateNames.size(), vxe::voxel::kAirState);
    m_catalog.m_tallLower.assign(m_catalog.m_stateNames.size(), vxe::voxel::kAirState);
    for (const auto& [lower, upper] : m_tallTops) {
        const auto below = m_catalog.defaultState(lower);
        const auto above = m_catalog.defaultState(upper);
        if (!below || !above) {
            problem(lower + ".toml", std::format("'tall_top' names an unknown block '{}'", upper));
            continue;
        }
        m_catalog.m_tallUpper.at(*below) = *above;
        m_catalog.m_tallLower.at(*above) = *below;
        // The lower half of a tall plant reaches into the upper half.
        m_catalog.m_aimBoxes.at(*below)[4] = 16;
    }
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
    m_catalog.m_textureLooks.push_back(0);
    m_lookSet.push_back(false);
    m_layers.emplace(key, layer);
    return layer;
}

// A channel of a color times the same channel of a tint. Multiplying sRGB
// values is close to multiplying linear ones, since the curve is nearly a
// power, and these colors are averages seen from far away.
std::vector<u32> BlockCatalog::shownColors(std::span<const u32> averageColors) const {
    const auto multiply = [](u32 color, u32 tint) {
        u32 result = color & 0xFF000000U;
        for (const u32 shift : {0U, 8U, 16U}) {
            const u32 channel = (((color >> shift) & 255U) * ((tint >> shift) & 255U)) / 255U;
            result |= channel << shift;
        }
        return result;
    };
    std::vector<u32> shown(averageColors.begin(), averageColors.end());
    const usize count = std::min(shown.size(), m_textureLooks.size());
    for (usize layer = 0; layer < count; ++layer) {
        const u32 look = m_textureLooks[layer];
        shown[layer] = multiply(averageColors[layer], m_tintColors.at((look >> 16U) & 15U));
        const u32 overlayLayer = look & 0xFFFFU;
        if (overlayLayer != 0 && overlayLayer - 1 < averageColors.size()) {
            const u32 overlay =
                multiply(averageColors[overlayLayer - 1], m_tintColors.at((look >> 20U) & 15U));
            const u32 cover = overlay >> 24U;
            u32 mixed = shown[layer] & 0xFF000000U;
            for (const u32 shift : {0U, 8U, 16U}) {
                const u32 base = (shown[layer] >> shift) & 255U;
                const u32 top = (overlay >> shift) & 255U;
                mixed |= (((base * (255U - cover)) + (top * cover)) / 255U) << shift;
            }
            shown[layer] = mixed;
        }
    }
    return shown;
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
