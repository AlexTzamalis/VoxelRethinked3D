#pragma once

#include <vxe/core/containers.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>

#include <array>
#include <atomic>
#include <utility>
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
// With a job system (setJobSystem) the search runs on a worker while the
// main thread keeps using the previous result, so streaming, which changes
// chunks nearly every frame, costs the main thread only a copy of the chunk
// list. Until the result for the camera's current chunk is in (normally the
// next frame), every chunk counts as visible: a stale result could hide what
// the new position shows. Chunks that changed since the search started use
// the old result for that time, which errs towards visible for new chunks
// (outside the box or behind open space) and lags one search for edits.
//
// Thread safety: main thread only; the search job touches only the back
// buffer, which the main thread does not touch while the job runs.
class CaveCuller {
public:
    // The largest box searched, in chunks. Beyond this (a camera thousands
    // of chunks from the world) the culler gives up and reports everything
    // visible.
    static constexpr usize kMaxCells = usize{1} << 22U;

    CaveCuller() = default;
    // Waits for a running search.
    ~CaveCuller();
    CaveCuller(const CaveCuller&) = delete;
    CaveCuller& operator=(const CaveCuller&) = delete;
    CaveCuller(CaveCuller&&) = delete;
    CaveCuller& operator=(CaveCuller&&) = delete;

    // Searches on `jobs` from now on, or on the calling thread with nullptr.
    // Waits for a running search first, so call it with nullptr before the
    // job system goes away.
    void setJobSystem(jobs::JobSystem* jobs);

    // Tells the culler how a chunk connects. kChunkAllConnected is the same
    // as removing it.
    void setChunk(ChunkPos position, u16 connectivity);
    void removeChunk(ChunkPos position);
    [[nodiscard]] usize knownChunks() const noexcept { return m_chunks.size(); }

    // Searches again if the camera chunk or any chunk changed since the last
    // search: right away without a job system, else by starting a job (if
    // none runs) and taking a finished job's result. Returns true when the
    // answers of isVisible() changed.
    bool update(ChunkPos camera);
    // Whether the latest search reached the chunk. Chunks outside the
    // searched box, and every chunk when the search gave up or its result is
    // for another camera chunk, are visible.
    [[nodiscard]] bool isVisible(ChunkPos position) const noexcept;
    // Bumped whenever the answers of isVisible() may have changed, so users
    // know when to rebuild what they derive.
    [[nodiscard]] u64 generation() const noexcept { return m_generation; }
    // A search job runs.
    [[nodiscard]] bool isSearching() const noexcept { return m_running; }
    // Chunks the latest search reached (including empty space).
    [[nodiscard]] usize reachedCells() const noexcept { return m_front.reached; }
    [[nodiscard]] f64 lastSearchMilliseconds() const noexcept { return m_front.searchMs; }

private:
    // One search: its copy of the chunks, the grid built from them and the
    // result. There are two: the front one answers isVisible(), the back one
    // is filled by the job, then they swap.
    struct Search {
        std::vector<std::pair<ChunkPos, u16>> chunks;
        u64 chunksVersion = ~u64{0}; // CaveCuller::m_version the copy was made at.
        u64 gridVersion = ~u64{0};   // chunksVersion when the grid was built.
        bool searched = false;
        bool gaveUp = false;
        ChunkPos camera{};
        // The searched box and, per cell, the faces it was entered through
        // (bits 0 to 5; 0 means never reached).
        ChunkPos origin{};
        std::array<i32, 3> size{};
        std::vector<u16> connectivity;
        std::vector<u8> entered;
        struct Step {
            u32 cell;
            i32 x; // The cell in the box.
            i32 y;
            i32 z;
            u8 enteredFace;
            u8 directions; // Bit d: the path moved towards face d at some point.
        };
        std::vector<Step> queue;
        usize reached = 0;
        f64 searchMs = 0.0;

        [[nodiscard]] bool cellIndex(ChunkPos position, usize& index) const noexcept;
        [[nodiscard]] bool buildGrid(ChunkPos from);
        void run(ChunkPos viewChunk);
        [[nodiscard]] bool isVisible(ChunkPos position) const noexcept;
    };

    void copyChunks(Search& search);

    HashMap<ChunkPos, u16> m_chunks; // Only chunks that are not fully connected.
    u64 m_version = 0;               // Bumped when m_chunks changes.
    Search m_front;
    Search m_back;
    jobs::JobSystem* m_jobs = nullptr;
    jobs::JobCounter m_counter;
    std::atomic<bool> m_backDone{false};
    bool m_running = false;
    bool m_hasCamera = false;
    ChunkPos m_camera{}; // The camera of the latest update().
    u64 m_generation = 0;
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
