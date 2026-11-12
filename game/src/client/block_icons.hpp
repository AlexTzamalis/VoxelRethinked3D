#pragma once

#include "common/block_catalog.hpp"

#include <vxe/core/types.hpp>
#include <vxe/render/texture_pack.hpp>

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vr {

// Little 3D blocks for the hotbar and the creative inventory (G6.4b), drawn
// from the block textures with their tints and overlays, so they follow the
// texture pack and need no art of their own.
//
// Each block is a 32 x 32 image: the top as a diamond, with the two sides
// below it, a little darker each so the shape reads. Baked on the CPU into the
// UI atlas, so drawing an icon costs one quad.
//
// Thread safety: main thread only.
class BlockIcons {
public:
    static constexpr vxe::u32 kSize = 32;

    struct Icon {
        std::string name;             // In the UI atlas: "icon/stone".
        std::vector<vxe::u32> pixels; // kSize x kSize, RGBA with red lowest, top row first.
    };
    // Finds a block texture of the packs by key, or nullptr.
    using FindTexture = std::function<const vxe::render::PackImage*(std::string_view key)>;

    // Bakes every visible block. Call again when the textures change.
    void bake(const BlockCatalog& catalog, const FindTexture& find);

    [[nodiscard]] const std::vector<Icon>& icons() const noexcept { return m_icons; }
    // The atlas name of a state's icon: all states of a block share one.
    // Empty for air and states without an icon.
    [[nodiscard]] std::string_view iconOf(vxe::voxel::BlockStateId state) const;

    // The six faces of a state (+X, -X, +Y, -Y, +Z, -Z) as 16 x 16 pictures
    // with their tints and overlays, for the item in the player's hand. A plant
    // gives its one picture in the first.
    [[nodiscard]] static std::array<std::vector<vxe::u32>, 6>
    faces(const BlockCatalog& catalog, vxe::voxel::BlockStateId state, const FindTexture& find);

private:
    void bakePlant(const BlockCatalog& catalog, const FindTexture& find, const BlockDefinition& block,
                   const vxe::voxel::BlockRenderInfo& info);

    std::vector<Icon> m_icons;
    std::vector<vxe::u32> m_iconOfState; // Index into m_icons plus 1, per state.
};

} // namespace vr
