#pragma once

#include <vxe/core/containers.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>

#include <array>
#include <vector>

namespace vxe::render {

// Chunk faces in the mesher's order (voxel::BlockFace): +X, -X, +Y, -Y, +Z,
// -Z. The opposite of face f is f ^ 1.
inline constexpr u32 kChunkFaceCount = 6;

// The bit of a chunk's 15 bit connectivity mask (voxel::ChunkMesh) that says
// faces a and b are joined by non opaque blocks. render sits below voxel in
// the module layers (ADR-015), so this repeats voxel::connectivityBit; a
// voxel test checks that both agree for every pair.
[[nodiscard]] constexpr u16 chunkConnectivityBit(u32 a, u32 b) noexcept {
    const u32 low = a < b ? a : b;
    const u32 high = a < b ? b : a;
    // Pairs (0,1) to (0,5) are bits 0 to 4, (1,2) to (1,5) bits 5 to 8, and so on.
    const u32 before = (low * (11U - low)) / 2U;
    return static_cast<u16>(1U << (before + high - low - 1U));
}
// Every face pair connected: an all air chunk, or one the renderer knows nothing about.
inline constexpr u16 kChunkAllConnected = 0x7FFF;

// The tight box around a chunk's quads, in chunk local block corners (0 to 16).
struct ChunkBounds {
    std::array<u8, 3> min{0, 0, 0};
    std::array<u8, 3> max{16, 16, 16};

    // 5 bits per value, min x y z then max x y z, for the shader's chunk metadata.
    [[nodiscard]] constexpr u32 pack() const noexcept {
        return u32{min[0]} | (u32{min[1]} << 5U) | (u32{min[2]} << 10U) | (u32{max[0]} << 15U) |
               (u32{max[1]} << 20U) | (u32{max[2]} << 25U);
    }
};

// Cave culling (Section 8.3, step 2): which chunks the camera can possibly
// see through the open space of the world.
//
// Every chunk mesh says which pairs of its faces are connected through non
// opaque blocks (a 15 bit mask). A breadth first search starts in the
// camera's chunk and steps into a neighbor only if the current chunk joins
// the face it was entered through to the face it leaves through. Steps never
// go back against a direction already taken (a path that went +X never goes
// -X), which keeps the search from bending around corners the eye cannot see
// around. Every chunk the search reaches is visible; the rest is hidden
// behind solid terrain, which is most of the underground in a cave and most
// caves seen from the surface.
//
// Chunks the culler was never told about count as open (air), so missing
// data can only make more chunks visible, never fewer. The search covers the
// box of known chunks, grown to include the camera and one chunk of margin
// for paths along the outside. The camera's own chunk is entered from every
// side at once.
//
// A chunk is searched again when it is entered through a face it was not
// entered through before, so the order of the search cannot hide chunks a
// different entry would have reached.
//
// The search runs only when the camera moves into another chunk or a chunk
// changes. In release, for a 65 x 16 x 65 chunk world (render distance 32):
// 0.46 ms when only the camera moved (the grid of cells is reused), about
// 1.1 ms after chunks changed (stat Render.Culling.CaveSearchMs).
//
// Thread safety: main thread only.
class CaveCuller {
public:
    // The largest box searched, in chunks. Beyond this (a camera thousands
    // of chunks from the world) the culler gives up and reports everything
    // visible.
    static constexpr usize kMaxCells = usize{1} << 22U;

    // Tells the culler how a chunk connects. kChunkAllConnected is the same
    // as removing it.
    void setChunk(ChunkPos position, u16 connectivity);
    void removeChunk(ChunkPos position);
    [[nodiscard]] usize knownChunks() const noexcept { return m_chunks.size(); }

    // Searches again if the camera chunk or any chunk changed since the last
    // search. Returns true if it searched.
    bool update(ChunkPos camera);
    // Whether the latest search reached the chunk. Chunks outside the
    // searched box, and every chunk when the search gave up, are visible.
    [[nodiscard]] bool isVisible(ChunkPos position) const noexcept;
    // Bumped by every search, so users know when to rebuild what they derive.
    [[nodiscard]] u64 generation() const noexcept { return m_generation; }
    // Chunks the latest search reached (including empty space).
    [[nodiscard]] usize reachedCells() const noexcept { return m_reached; }
    [[nodiscard]] f64 lastSearchMilliseconds() const noexcept { return m_searchMs; }

private:
    [[nodiscard]] bool cellIndex(ChunkPos position, usize& index) const noexcept;
    [[nodiscard]] bool buildGrid(ChunkPos camera);
    void search(ChunkPos camera);

    HashMap<ChunkPos, u16> m_chunks; // Only chunks that are not fully connected.
    bool m_dirty = true;             // Chunks changed since the grid was built.
    bool m_searched = false;
    bool m_gaveUp = false;
    ChunkPos m_camera{};
    // The searched box and, per cell, the faces it was entered through
    // (bits 0 to 5; 0 means never reached).
    ChunkPos m_origin{};
    std::array<i32, 3> m_size{};
    std::vector<u16> m_connectivity;
    std::vector<u8> m_entered;
    struct Step {
        u32 cell;
        i32 x; // The cell in the box.
        i32 y;
        i32 z;
        u8 enteredFace;
        u8 directions; // Bit d: the path moved towards face d at some point.
    };
    std::vector<Step> m_queue;
    usize m_reached = 0;
    u64 m_generation = 0;
    f64 m_searchMs = 0.0;
};

// The face direction test (Section 8.3): a group of faces that all point
// along `face` can only be seen from the side they point to. `boxMin` and
// `boxMax` are the chunk's bounds relative to the camera (the camera at 0).
[[nodiscard]] inline bool faceGroupFacesCamera(u32 face, const Vec3& boxMin, const Vec3& boxMax) noexcept {
    const u32 axis = face >> 1U;
    // A +X face at plane x is seen from x' > x; the lowest such plane in the
    // chunk is at boxMin. -X faces are seen from below the highest plane.
    return (face & 1U) == 0U ? boxMin[static_cast<i32>(axis)] < 0.0F : boxMax[static_cast<i32>(axis)] > 0.0F;
}

} // namespace vxe::render
