#pragma once

#include <vxe/core/types.hpp>
#include <vxe/voxel/lod_tile.hpp>

#include <array>
#include <utility>
#include <vector>

namespace vxe::voxel {

// One quad of far terrain, 12 bytes (Section 13.5 asks for 8; heights need
// more room than chunk quads: a side face spans hundreds of blocks and sits
// anywhere in a 4096 block tall world, so three words instead of two):
//
//   word0  bits  0..6   x in cells (0..64)
//          bits  7..13  z in cells (0..64)
//          bits 14..19  size along U, minus 1 (cells, 0..63)
//          bits 20..25  size along V, minus 1 (cells, top and bottom faces only)
//          bits 26..28  face (+X, -X, +Y, -Y, +Z, -Z)
//   word1  bits  0..23  y in blocks above the mesh's base Y
//          bits 24..27  sky light (0..15)
//   word2  bits  0..19  height in blocks (side faces; 0 for tops and bottoms)
//          bits 20..31  material (index into the LOD color table)
//
// Top and bottom faces lie at y and cover sizeU x sizeV cells (U along X, V
// along Z). Side faces stand at the cell edge x (or z), cover sizeU cells
// along the edge (Z for X faces, X for Z faces) and go from y up to y +
// height. For +X and +Z faces the edge is at x + 1 (z + 1).
struct LodQuad {
    std::array<u32, 3> words{};

    [[nodiscard]] u32 x() const noexcept { return words[0] & 0x7FU; }
    [[nodiscard]] u32 z() const noexcept { return (words[0] >> 7U) & 0x7FU; }
    [[nodiscard]] u32 sizeU() const noexcept { return ((words[0] >> 14U) & 0x3FU) + 1; }
    [[nodiscard]] u32 sizeV() const noexcept { return ((words[0] >> 20U) & 0x3FU) + 1; }
    [[nodiscard]] u32 face() const noexcept { return (words[0] >> 26U) & 7U; }
    [[nodiscard]] u32 y() const noexcept { return words[1] & 0xFFFFFFU; }
    [[nodiscard]] u32 skyLight() const noexcept { return (words[1] >> 24U) & 15U; }
    [[nodiscard]] u32 height() const noexcept { return words[2] & 0xFFFFFU; }
    [[nodiscard]] u32 material() const noexcept { return words[2] >> 20U; }
};
static_assert(sizeof(LodQuad) == 12);

inline constexpr u32 kLodMaxMaterial = 0xFFFU;
inline constexpr u32 kLodMaxRelativeY = 0xFFFFFFU;
inline constexpr u32 kLodMaxHeight = 0xFFFFFU;

// A tile's quads, ready for the LOD renderer.
struct LodMesh {
    LodTileKey key{};
    i32 baseY = 0; // Quad y values count from here.
    i32 minY = 0;  // The lowest and highest block the quads touch, for culling.
    i32 maxY = 0;
    std::vector<LodQuad> quads;
};

struct LodMeshOptions {
    // How far skirts reach down at tile edges, in cells of the tile's level
    // (at least 16 blocks). Neighbor tiles may be of another level and sit a
    // little higher or lower; the skirt hides the crack between them.
    u32 skirtCells = 4;
};

// Turns a tile's spans into quads (Section 13.5), like voxel meshing at a
// coarser scale: the top of every span (the highest spans of neighboring
// cells merged greedily into rectangles), the underside of spans with
// another span below (overhangs, floating islands), and the parts of span
// sides that no span of the neighbor cell covers, merged along the edge. At
// the tile's edges the neighbor is unknown, so the side goes down as a skirt.
//
// Thread safety: a LodMesher holds scratch memory; use one per thread.
class LodMesher {
public:
    void mesh(const LodTile& tile, const LodMeshOptions& options, LodMesh& out);

private:
    // A quad with an absolute y, before packing.
    struct Raw {
        u8 face = 0;
        u8 x = 0;
        u8 z = 0;
        u8 sizeU = 1;
        u8 sizeV = 1;
        u8 sky = 15;
        u16 material = 0;
        i32 y = 0;
        i32 height = 0;
    };
    void addTops(const LodTile& tile);
    [[nodiscard]] std::pair<i32, i32> growTop(const LodTile& tile, i32 cx, i32 cz);
    void addLowerFaces(const LodTile& tile);
    void addSides(const LodTile& tile, const LodMeshOptions& options);
    void mergeSides();
    static void pack(std::span<const Raw> raws, LodMesh& out);

    std::vector<Raw> m_raw;
    std::vector<Raw> m_sides;
    std::array<bool, kLodCellCount> m_done{};
};

} // namespace vxe::voxel
