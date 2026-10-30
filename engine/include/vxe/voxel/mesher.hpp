#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/chunk_snapshot.hpp>

#include <array>
#include <span>
#include <vector>

namespace vxe::voxel {

// One quad of a chunk mesh in 8 bytes (Section 10.4). The vertex shader
// builds the 4 corners from it (vertex pulling); the chunk position comes
// from the chunk metadata buffer.
//
// word0: bits 0..4 x, 5..9 y, 10..14 z (0 to 16: the quad's minimum corner
//        in chunk local block corners; a face can sit on the far edge 16),
//        15..18 width - 1, 19..22 height - 1, 23..25 face (BlockFace),
//        26 flip (AO diagonal), 27..31 reserved. Face 6 and 7 are the two
//        diagonal planes of a plant: 27..28 and 29..30 shift it on X and Z,
//        and 31 makes it a large bushy plane around the block's center.
// word1: bits 0..13 texture layer, 14..21 AO (2 bits per corner, 3 is fully
//        lit), 22..25 sky light, 26..29 block light, 30..31 reserved.
//
// Width runs along the face's U axis and height along its V axis:
//   +X, -X: U = Z, V = Y      +Y, -Y: U = X, V = Z      +Z, -Z: U = X, V = Y
// Corners are numbered c0 (u0, v0), c1 (u1, v0), c2 (u1, v1), c3 (u0, v1);
// AO corner i is in bits 14 + 2i. The default triangles share the c0 to c2
// diagonal; flip moves it to c1 to c3. The corners run counterclockwise
// seen from the side U x V points to, which is -X, -Y and +Z; the shader
// reverses the winding for +X, +Y and -Z faces so every face is front
// facing from outside (ADR-016).
struct PackedQuad {
    u32 word0 = 0;
    u32 word1 = 0;

    [[nodiscard]] u32 x() const noexcept { return word0 & 31U; }
    [[nodiscard]] u32 y() const noexcept { return (word0 >> 5U) & 31U; }
    [[nodiscard]] u32 z() const noexcept { return (word0 >> 10U) & 31U; }
    [[nodiscard]] u32 width() const noexcept { return ((word0 >> 15U) & 15U) + 1; }
    [[nodiscard]] u32 height() const noexcept { return ((word0 >> 19U) & 15U) + 1; }
    [[nodiscard]] BlockFace face() const noexcept { return static_cast<BlockFace>((word0 >> 23U) & 7U); }
    [[nodiscard]] bool flipped() const noexcept { return ((word0 >> 26U) & 1U) != 0; }
    [[nodiscard]] u32 textureLayer() const noexcept { return word1 & 0x3FFFU; }
    // AO of corner 0 to 3: 0 is the darkest, 3 is not occluded.
    [[nodiscard]] u32 ao(u32 corner) const noexcept { return (word1 >> (14U + (2U * corner))) & 3U; }
    [[nodiscard]] u32 skyLight() const noexcept { return (word1 >> 22U) & 15U; }
    [[nodiscard]] u32 blockLight() const noexcept { return (word1 >> 26U) & 15U; }

    friend constexpr bool operator==(const PackedQuad&, const PackedQuad&) = default;
};
static_assert(sizeof(PackedQuad) == 8);

// The normal, U and V axes of a face as axis numbers (0 X, 1 Y, 2 Z), and
// whether the face points along its axis' positive direction.
struct FaceAxes {
    u32 normal;
    u32 u;
    u32 v;
    bool positive;
};
[[nodiscard]] constexpr FaceAxes faceAxes(BlockFace face) noexcept {
    switch (face) {
    case BlockFace::PosX:
        return {.normal = 0, .u = 2, .v = 1, .positive = true};
    case BlockFace::NegX:
        return {.normal = 0, .u = 2, .v = 1, .positive = false};
    case BlockFace::PosY:
        return {.normal = 1, .u = 0, .v = 2, .positive = true};
    case BlockFace::NegY:
        return {.normal = 1, .u = 0, .v = 2, .positive = false};
    case BlockFace::PosZ:
        return {.normal = 2, .u = 0, .v = 1, .positive = true};
    case BlockFace::NegZ:
        break;
    }
    return {.normal = 2, .u = 0, .v = 1, .positive = false};
}

// The render passes a mesh feeds: opaque, cutout (alpha tested), plants
// (crossed planes seen from both sides, in the first group of their layer)
// and translucent (blended, sorted) quads go into separate lists (Section 10.5).
enum class MeshLayer : u8 {
    Opaque,
    Cutout,
    Plants,
    Translucent,
};
inline constexpr usize kMeshLayerCount = 4;
// One group per layer and face direction, so whole groups facing away from
// the camera can be skipped (Section 8.2).
inline constexpr usize kFaceGroupCount = kMeshLayerCount * kBlockFaceCount;

[[nodiscard]] constexpr usize faceGroupIndex(MeshLayer layer, BlockFace face) noexcept {
    return (static_cast<usize>(layer) * kBlockFaceCount) + static_cast<usize>(face);
}

// The bit of a connectivity mask that says faces a and b of a chunk are
// connected through blocks that are not opaque (cave culling, Section 8.3).
// 15 bits, one per unordered pair of the 6 faces; a and b must differ.
[[nodiscard]] constexpr u16 connectivityBit(BlockFace a, BlockFace b) noexcept {
    auto low = static_cast<u32>(a);
    auto high = static_cast<u32>(b);
    if (low > high) {
        const u32 swap = low;
        low = high;
        high = swap;
    }
    // Pairs (0,1) to (0,5) are bits 0 to 4, (1,2) to (1,5) bits 5 to 8, and so on.
    const u32 before = (low * (11U - low)) / 2U;
    return static_cast<u16>(1U << (before + high - low - 1U));
}
inline constexpr u16 kAllConnected = 0x7FFF;

// The output of meshing one chunk. The owner reuses it between chunks, so
// after warm up meshing does not allocate.
struct ChunkMesh {
    ChunkPos position{};
    u64 version = 0; // The snapshot's version; compare before uploading.
    // All quads, ordered by group (layer major, then face). Group g is
    // quads[groupBegin[g]] to quads[groupBegin[g + 1]].
    std::vector<PackedQuad> quads;
    std::array<u32, kFaceGroupCount + 1> groupBegin{};
    u16 connectivity = 0;
    // The tight bounds of all quads in chunk local block corners (0 to 16);
    // min > max on an axis when the mesh is empty.
    std::array<u8, 3> boundsMin{};
    std::array<u8, 3> boundsMax{};

    [[nodiscard]] std::span<const PackedQuad> group(MeshLayer layer, BlockFace face) const noexcept {
        const usize g = faceGroupIndex(layer, face);
        return std::span(quads).subspan(groupBegin[g], groupBegin[g + 1] - groupBegin[g]);
    }
    [[nodiscard]] std::span<const PackedQuad> layer(MeshLayer layer) const noexcept {
        const usize first = faceGroupIndex(layer, BlockFace::PosX);
        return std::span(quads).subspan(groupBegin[first],
                                        groupBegin[first + kBlockFaceCount] - groupBegin[first]);
    }
    [[nodiscard]] bool empty() const noexcept { return quads.empty(); }
};

// Choices that change the mesh. All need a remesh to take effect.
struct MeshOptions {
    bool greedyMerge = true;       // Merge equal neighboring faces into larger quads.
    bool mergeAcrossLight = false; // Merge faces even when their light differs (faster, coarser).
    bool fancyLeaves = true;       // Cutout blocks stay see through; off meshes them as opaque.
    bool advancedLeaves = false;   // Leaves grow bushy planes past their block.
    u64 variationSeed = 0;         // The world's seed: picks plant textures per position.

    friend constexpr bool operator==(const MeshOptions&, const MeshOptions&) = default;
};

// Registers Graphics.Terrain.GreedyMerge, MergeAcrossLight, FancyLeaves and
// AdvancedLeaves (all RequiresRemesh) and reads them into MeshOptions. The streaming
// manager reads options() once per batch of mesh jobs and remeshes resident
// chunks when they change.
class MesherSettings {
public:
    explicit MesherSettings(config::SettingsRegistry& settings);
    [[nodiscard]] MeshOptions options() const;

private:
    config::Setting<bool> m_greedyMerge;
    config::Setting<bool> m_mergeAcrossLight;
    config::Setting<bool> m_fancyLeaves;
    config::Setting<bool> m_advancedLeaves;
};

// Binary greedy mesher (Section 10): turns a chunk snapshot into packed
// quads with ambient occlusion, plus the cave culling connectivity mask and
// the tight bounds.
//
// How it works, for a reader new to it:
// 1. Each row of 18 blocks along X in the snapshot becomes a 32 bit mask of
//    opaque blocks, so whole rows are tested at once.
// 2. Visible faces come from bit operations on those masks: a +X face is
//    where a block is opaque and the block after it is not
//    (row & ~(row >> 1)); +Y and +Z faces compare a row with the row above
//    or behind it. That replaces 6 neighbor checks per block with a few
//    instructions per row of 16 blocks.
// 3. Cutout and translucent blocks get their faces the same way (against
//    non opaque neighbors), then lose the faces they share with the same
//    state if it culls its own type (water next to water, glass next to
//    glass).
// 4. For each face direction and layer of 16 x 16 faces, a greedy merge
//    walks the set bits with countr_zero and grows rectangles as wide and
//    then as tall as faces with the same state, texture, AO and light
//    allow (Section 10.2).
// 5. A flood fill over rows of open blocks, again as bit masks, finds which
//    chunk faces connect through non opaque blocks.
// The simple alternative, a loop over all 4096 blocks testing 6 neighbors
// each and emitting one quad per face, is 10 to 50 times slower and emits
// several times more quads.
//
// Light is not computed yet (E10): quads carry full sky light and no block
// light, which keeps the merge rules and the format final already.
//
// Stats (optional): Voxel.Mesher.Chunks, Voxel.Mesher.Quads,
// Voxel.Mesher.Time.
//
// Thread safety: a Mesher holds scratch memory, so each worker thread owns
// one; meshing different snapshots on different Meshers is safe. The
// registry view is read only.
class Mesher {
public:
    explicit Mesher(stats::StatRegistry* stats = nullptr);

    // Meshes `snapshot` into `out` (replacing its contents). `blocks` must
    // stay alive during the call; the job holds it (BlockRegistry::current).
    void mesh(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks, const MeshOptions& options,
              ChunkMesh& out);

private:
    // Per state classification, rebuilt when the registry view or the
    // options change.
    enum class Kind : u8 {
        Invisible,
        Opaque,
        Cutout,
        Translucent,
    };
    void prepare(const BlockRegistryView& blocks, const MeshOptions& options);
    void buildMasks(const ChunkSnapshot& snapshot);
    void buildSameStateMasks(const ChunkSnapshot& snapshot);
    void findFaces();
    void addFaces(usize layer, BlockFace face, usize y, usize z, u32 bits);
    [[nodiscard]] u64 faceKey(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks, BlockFace face,
                              u32 slice, u32 u, u32 v) const noexcept;
    void mergeFaces(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks,
                    const MeshOptions& options, ChunkMesh& out);
    // What growing a rectangle needs besides the face position.
    struct MergeInput {
        const ChunkSnapshot& snapshot;
        const BlockRegistryView& blocks;
        u64 compareMask; // Key bits that must match for faces to merge.
        bool greedy;
    };
    struct Rectangle {
        u32 width;
        u32 height;
        u32 word1; // The first face's quad word1 (texture, AO, light).
    };
    using FaceRows = std::array<u16, 16>;
    // Grows a rectangle from the face at (u, v) of a slice and clears its
    // faces from `rows`.
    [[nodiscard]] Rectangle growRectangle(const MergeInput& in, BlockFace face, u32 slice, FaceRows& rows,
                                          u32 u, u32 v) const noexcept;
    using OpenRows = std::array<u16, kChunkArea>;
    [[nodiscard]] u32 floodComponent(usize start, const OpenRows& open, OpenRows& visited);
    // Appends the crossed planes of plants and bushy leaves.
    void addPlants(const ChunkSnapshot& snapshot, const BlockRegistryView& blocks, const MeshOptions& options,
                   ChunkMesh& out);
    [[nodiscard]] u16 connectivity();

    // One byte per possible state: its Kind, plus bit 2 if it hides faces
    // against its own state. Sized for every id so lookups need no check.
    std::vector<u8> m_classes;
    usize m_preparedSize = 0;
    const BlockRegistryView* m_preparedView = nullptr;
    u64 m_preparedGeneration = 0;
    MeshOptions m_preparedOptions{};

    // Masks of each snapshot row along X, bit x for snapshot x 0 to 17,
    // index y * 18 + z: opaque, cutout, translucent and "culls its own
    // state" blocks, and blocks equal to their neighbor at x + 1, y + 1 and
    // z + 1 (only built when the snapshot has see through blocks).
    std::array<u32, kSnapshotArea> m_opaque{};
    std::array<u32, kSnapshotArea> m_cutout{};
    std::array<u32, kSnapshotArea> m_translucent{};
    std::array<u32, kSnapshotArea> m_cullsSame{};
    std::array<u32, kSnapshotArea> m_sameX{};
    std::array<u32, kSnapshotArea> m_sameY{};
    std::array<u32, kSnapshotArea> m_sameZ{};
    bool m_hasSeeThrough = false;
    bool m_hasPlants = false; // A state with kPlantBit is in the snapshot.

    // Visible faces per layer, face, slice (chunk local coordinate along the
    // normal) and row (along V), bit u for U 0 to 15. Merging consumes every
    // bit, so the array is all zero again after each mesh() and never needs
    // clearing. m_groupSlices marks the slices of each group that have faces.
    std::array<std::array<std::array<FaceRows, 16>, kBlockFaceCount>, kMeshLayerCount> m_faces{};
    std::array<u32, kFaceGroupCount> m_groupSlices{};
    std::vector<u32> m_floodStack;

    stats::StatRegistry* m_stats;
    stats::StatId m_chunksStat;
    stats::StatId m_quadsStat;
    stats::StatId m_timeStat;
};

// Sorts translucent quads back to front for a viewer at `viewer` (chunk
// local block units, may lie outside the chunk), farthest first, so blended
// water and glass draw correctly. The renderer calls it when the camera
// moves into another chunk (Section 10.5).
void sortBackToFront(std::span<PackedQuad> quads, const Vec3& viewer);

} // namespace vxe::voxel
