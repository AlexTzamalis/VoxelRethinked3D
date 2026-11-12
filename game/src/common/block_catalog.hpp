#pragma once

#include <vxe/core/containers.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vr {

// One block definition file, as text, with the name used in error messages.
struct BlockSource {
    std::string file; // For example "stone.toml".
    std::string text;
};

// A block (Section 9.3): a name and its states. Most blocks have one state;
// blocks with variants (the axis of a log) have one per variant.
struct BlockDefinition {
    std::string name;        // Stable, for example "oak_log".
    std::string displayName; // "Oak Log".
    std::string file;        // The definition file it came from.
    std::string category;    // Its tab in the creative inventory.
    bool hidden = false;     // Not in the inventory: the top half of a tall plant.
    vxe::voxel::BlockStateId firstState = 0;
    vxe::u16 stateCount = 1;
};

// Every block the game knows, loaded from game/assets/data/blocks/*.toml
// (G1.2, ADR-009) and turned into what the engine needs.
//
// - State ids: air is 0, then every state of every block in file name
//   order, so the same files always give the same ids. Ids are runtime only;
//   saves store state names (Section 9.3), which stateName() and stateId()
//   map both ways.
// - State names are the block name, plus "[key=value,...]" with the keys in
//   alphabetical order for blocks with variants: "oak_log[axis=y]".
// - Texture keys are given texture array layers in first use order
//   (textureKeys()). The texture pack loader (G2.2) builds the array in the
//   same order, replacing missing textures with a placeholder.
//
// Loading checks every file and reports all problems at once, each with the
// file and the field, so a broken data file is quick to fix.
//
// Thread safety: built once at startup on the main thread, then read only.
class BlockCatalog {
public:
    [[nodiscard]] static vxe::Result<BlockCatalog> load(std::span<const BlockSource> sources);
    // Reads every *.toml file of a folder (not recursive).
    [[nodiscard]] static vxe::Result<BlockCatalog> loadFolder(const std::filesystem::path& folder);

    [[nodiscard]] vxe::usize blockCount() const noexcept { return m_blocks.size(); }
    // Including air.
    [[nodiscard]] vxe::usize stateCount() const noexcept { return m_stateNames.size(); }

    [[nodiscard]] const BlockDefinition* block(std::string_view name) const;
    [[nodiscard]] std::span<const BlockDefinition> blocks() const noexcept { return m_blocks; }
    [[nodiscard]] std::optional<vxe::voxel::BlockStateId> stateId(std::string_view stateName) const;
    [[nodiscard]] std::string_view stateName(vxe::voxel::BlockStateId state) const;
    // The first (default) state of a block, or nothing for unknown names.
    [[nodiscard]] std::optional<vxe::voxel::BlockStateId> defaultState(std::string_view blockName) const;
    [[nodiscard]] const vxe::voxel::BlockRenderInfo& renderInfo(vxe::voxel::BlockStateId state) const {
        return m_renderInfos.at(state);
    }

    // Whether the player and blocks collide with a state; liquids are not
    // solid, slow the player down and let blocks replace them.
    [[nodiscard]] bool isSolid(vxe::voxel::BlockStateId state) const {
        return (m_stateFlags.at(state) & 1U) != 0;
    }
    [[nodiscard]] bool isLiquid(vxe::voxel::BlockStateId state) const {
        return (m_stateFlags.at(state) & 2U) != 0;
    }
    // Standing in water (waterlogged in the block file): corals, kelp.
    [[nodiscard]] bool isWaterlogged(vxe::voxel::BlockStateId state) const {
        return m_renderInfos.at(state).fluid != 0;
    }
    // Liquid fills the cell: a liquid, or a block standing in one. The player
    // swims there.
    [[nodiscard]] bool isWet(vxe::voxel::BlockStateId state) const {
        return isLiquid(state) || isWaterlogged(state);
    }
    // What is left when the block goes: the water a waterlogged block stood
    // in, or air.
    [[nodiscard]] vxe::voxel::BlockStateId emptied(vxe::voxel::BlockStateId state) const {
        return m_renderInfos.at(state).fluid;
    }
    // Placing a block replaces it (plants). A slab only while it is one snow
    // layer thin (2 sixteenths), as a deeper one is worth keeping.
    [[nodiscard]] bool isReplaceable(vxe::voxel::BlockStateId state) const {
        const vxe::u8 height = m_renderInfos.at(state).height;
        return (m_stateFlags.at(state) & 4U) != 0 && (height == 0 || height <= 2);
    }
    // How tall the block stands in its cell, 0 to 1: 1 for whole blocks, less
    // for slabs (snow layers). Shapes the outline and, for solid blocks, collision.
    [[nodiscard]] vxe::f64 shapeHeight(vxe::voxel::BlockStateId state) const {
        const vxe::u8 height = m_renderInfos.at(state).height;
        return height == 0 ? 1.0 : static_cast<vxe::f64>(height) / 16.0;
    }
    // The height the player collides with, 0 for blocks that are not solid.
    [[nodiscard]] vxe::f64 collisionHeight(vxe::voxel::BlockStateId state) const {
        return isSolid(state) ? shapeHeight(state) : 0.0;
    }
    // The box a block fills in its cell, in sixteenths: x0, y0, z0, x1, y1, z1.
    // Aiming and the outline use it, so a flower is a small target and the
    // block behind it can be aimed at past it. From aim_box in the block file,
    // or by kind: plants a small box, slabs their height, others the whole cell.
    using AimBox = std::array<vxe::u8, 6>;
    [[nodiscard]] const AimBox& aimBox(vxe::voxel::BlockStateId state) const { return m_aimBoxes.at(state); }
    // A slab one step taller of the same block (the next snow layer), or air.
    [[nodiscard]] vxe::voxel::BlockStateId nextLayer(vxe::voxel::BlockStateId state) const;
    // Whether two states belong to one block, like two snow layers.
    [[nodiscard]] bool sameBlock(vxe::voxel::BlockStateId a, vxe::voxel::BlockStateId b) const;
    // It goes away when the block below it does (plants).
    [[nodiscard]] bool needsSupport(vxe::voxel::BlockStateId state) const {
        return (m_stateFlags.at(state) & 8U) != 0;
    }
    // The other half of a tall plant, or air: the state that goes above a
    // lower half, and below an upper half.
    [[nodiscard]] vxe::voxel::BlockStateId tallUpper(vxe::voxel::BlockStateId state) const {
        return m_tallUpper.at(state);
    }
    [[nodiscard]] vxe::voxel::BlockStateId tallLower(vxe::voxel::BlockStateId state) const {
        return m_tallLower.at(state);
    }

    // Texture array layer i shows textureKeys()[i].
    [[nodiscard]] std::span<const std::string> textureKeys() const noexcept { return m_textureKeys; }
    // How each texture layer is colored, voxel::textureLook, for
    // ChunkRenderer::setTextureLooks.
    [[nodiscard]] std::span<const vxe::u32> textureLooks() const noexcept { return m_textureLooks; }
    // The color of each tint kind from the block files, 0xAABBGGRR. Biome
    // files replace this (G3.7).
    [[nodiscard]] std::span<const vxe::u32> tintColors() const noexcept { return m_tintColors; }
    // Each texture's average color (from the pack) as it shows on blocks,
    // with its tint and overlay, so far terrain matches the chunks.
    [[nodiscard]] std::vector<vxe::u32> shownColors(std::span<const vxe::u32> averageColors) const;

    // The engine's table of every state's render info, ready for
    // voxel::BlockRegistry::publish.
    [[nodiscard]] vxe::Result<std::shared_ptr<vxe::voxel::BlockRegistryView>> buildRegistry() const;

private:
    friend class BlockCatalogLoader;

    std::vector<BlockDefinition> m_blocks;
    std::vector<std::string> m_stateNames;
    std::vector<vxe::voxel::BlockRenderInfo> m_renderInfos;
    std::vector<AimBox> m_aimBoxes;
    std::vector<vxe::u8> m_stateFlags; // Per state: bit 0 solid, 1 liquid, 2 replaceable, 3 needs support.
    std::vector<vxe::voxel::BlockStateId> m_tallUpper;
    std::vector<vxe::voxel::BlockStateId> m_tallLower;
    std::vector<std::string> m_textureKeys;
    std::vector<vxe::u32> m_textureLooks;
    std::array<vxe::u32, vxe::voxel::kTintCount> m_tintColors{};
    vxe::StringMap<vxe::voxel::BlockStateId> m_stateIds;
    vxe::StringMap<vxe::usize> m_blockIndex;
};

} // namespace vr
