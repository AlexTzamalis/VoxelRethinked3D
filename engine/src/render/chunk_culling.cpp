#include "vxe/render/chunk_culling.hpp"

#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>

namespace vxe::render {

CaveCuller::~CaveCuller() {
    setJobSystem(nullptr);
}

void CaveCuller::setJobSystem(jobs::JobSystem* jobs) {
    if (m_running) {
        m_jobs->wait(m_counter);
        m_running = false;
        m_backDone.store(false, std::memory_order_relaxed);
    }
    m_jobs = jobs;
}

void CaveCuller::setChunk(ChunkPos position, u16 connectivity) {
    if (connectivity == kChunkAllConnected) {
        removeChunk(position);
        return;
    }
    const auto [it, inserted] = m_chunks.try_emplace(position, connectivity);
    if (inserted || it->second != connectivity) {
        it->second = connectivity;
        ++m_version;
    }
}

void CaveCuller::removeChunk(ChunkPos position) {
    if (m_chunks.erase(position) > 0) {
        ++m_version;
    }
}

void CaveCuller::copyChunks(Search& search) {
    if (search.chunksVersion != m_version) {
        search.chunks.assign(m_chunks.begin(), m_chunks.end());
        search.chunksVersion = m_version;
    }
}

bool CaveCuller::update(ChunkPos camera) {
    bool changed = false;
    if (!m_hasCamera || camera != m_camera) {
        m_hasCamera = true;
        m_camera = camera;
        changed = true; // Until a search for this chunk is in, everything is visible.
    }
    const auto needsSearch = [this, camera](const Search& search) {
        return !search.searched || search.gridVersion != m_version || search.camera != camera;
    };
    if (m_jobs == nullptr) {
        if (needsSearch(m_front)) {
            copyChunks(m_front);
            m_front.run(camera);
            changed = true;
        }
    } else {
        if (m_running && m_backDone.load(std::memory_order_acquire)) {
            m_running = false;
            std::swap(m_front, m_back);
            changed = true;
        }
        if (!m_running && needsSearch(m_front)) {
            // The back buffer holds an older search; bring its chunk copy up
            // to date here, since the job must not read m_chunks.
            copyChunks(m_back);
            m_running = true;
            m_backDone.store(false, std::memory_order_relaxed);
            m_jobs->schedule(
                m_counter,
                [this, camera] {
                    m_back.run(camera);
                    m_backDone.store(true, std::memory_order_release);
                },
                jobs::Priority::High);
        }
    }
    if (changed) {
        ++m_generation;
    }
    return changed;
}

bool CaveCuller::isVisible(ChunkPos position) const noexcept {
    if (m_front.camera != m_camera) {
        return true;
    }
    return m_front.isVisible(position);
}

bool CaveCuller::Search::cellIndex(ChunkPos position, usize& index) const noexcept {
    const i64 x = i64{position.x} - origin.x;
    const i64 y = i64{position.y} - origin.y;
    const i64 z = i64{position.z} - origin.z;
    if (x < 0 || y < 0 || z < 0 || x >= size[0] || y >= size[1] || z >= size[2]) {
        return false;
    }
    index = static_cast<usize>(x + (size[0] * (y + (i64{size[1]} * z))));
    return true;
}

bool CaveCuller::Search::isVisible(ChunkPos position) const noexcept {
    usize index = 0;
    if (!searched || gaveUp || !cellIndex(position, index)) {
        return true;
    }
    return entered[index] != 0;
}

// The box (every known chunk and the camera, plus one chunk of margin) and
// the connectivity of each of its cells. False when the culler gives up.
bool CaveCuller::Search::buildGrid(ChunkPos from) {
    gridVersion = chunksVersion;
    std::array<i64, 3> low{from.x, from.y, from.z};
    std::array<i64, 3> high = low;
    for (const auto& entry : chunks) {
        const std::array<i64, 3> p{entry.first.x, entry.first.y, entry.first.z};
        for (usize axis = 0; axis < 3; ++axis) {
            low.at(axis) = std::min(low.at(axis), p.at(axis));
            high.at(axis) = std::max(high.at(axis), p.at(axis));
        }
    }
    u64 cells = 1;
    for (usize axis = 0; axis < 3; ++axis) {
        low.at(axis) -= 1;
        high.at(axis) += 1;
        cells *= static_cast<u64>(high.at(axis) - low.at(axis) + 1);
    }
    gaveUp = chunks.empty() || cells > kMaxCells;
    if (gaveUp) {
        return false;
    }
    origin = {.x = static_cast<i32>(low[0]), .y = static_cast<i32>(low[1]), .z = static_cast<i32>(low[2])};
    for (usize axis = 0; axis < 3; ++axis) {
        size.at(axis) = static_cast<i32>(high.at(axis) - low.at(axis) + 1);
    }
    connectivity.assign(static_cast<usize>(cells), kChunkAllConnected);
    entered.assign(static_cast<usize>(cells), 0);
    for (const auto& [position, mask] : chunks) {
        usize index = 0;
        if (cellIndex(position, index)) {
            connectivity[index] = mask;
        }
    }
    return true;
}

void CaveCuller::Search::run(ChunkPos viewChunk) {
    VXE_PROFILE_FUNCTION();
    const Stopwatch stopwatch;
    searched = true;
    camera = viewChunk;
    usize start = 0;
    // When only the camera moved within the box, the grid still holds.
    if (gridVersion != chunksVersion || gaveUp || !cellIndex(viewChunk, start)) {
        if (!buildGrid(viewChunk)) {
            reached = 0;
            searchMs = stopwatch.elapsedMilliseconds();
            return;
        }
        static_cast<void>(cellIndex(viewChunk, start));
    }
    std::ranges::fill(entered, u8{0});

    // Steps between cells: +X, -X, +Y, -Y, +Z, -Z. Each queued step carries
    // its cell coordinates, so stepping needs no divisions.
    constexpr std::array<std::array<i32, 3>, kChunkFaceCount> kOffsets = {
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    const std::array<i64, kChunkFaceCount> stride = {
        1, -1, size[0], -size[0], i64{size[0]} * size[1], -(i64{size[0]} * size[1])};
    queue.clear();
    reached = 0;
    // Moves from `from` through `face` into the neighbor cell, unless that
    // leaves the box or the neighbor was already entered through that side.
    const auto step = [this, &kOffsets, &stride](const Step& from, u32 face, u32 directions) {
        const std::array<i32, 3>& offset = kOffsets.at(face);
        const std::array<i32, 3> next = {from.x + offset[0], from.y + offset[1], from.z + offset[2]};
        for (usize axis = 0; axis < 3; ++axis) {
            if (next.at(axis) < 0 || next.at(axis) >= size.at(axis)) {
                return;
            }
        }
        const auto cell = static_cast<u32>(static_cast<i64>(from.cell) + stride.at(face));
        const u32 enteredFace = face ^ 1U;
        u8& cellEntered = entered[cell];
        const auto bit = static_cast<u8>(1U << enteredFace);
        const u16 cellConnectivity = connectivity[cell];
        // A path never reverses along an axis, so the directions it moved in
        // are fixed by the cell's offset from the camera. A fully open cell
        // therefore expands the same exits whichever side it was entered
        // from: only the first entry matters. Only partly open cells, where
        // the entry face decides the exits, are searched once per side.
        if ((cellEntered & bit) != 0 || (cellEntered != 0 && cellConnectivity == kChunkAllConnected)) {
            return;
        }
        if (cellEntered == 0) {
            ++reached;
        }
        cellEntered = static_cast<u8>(cellEntered | bit);
        if (cellConnectivity == 0) {
            return; // Seen, but nothing is seen through it.
        }
        queue.push_back({.cell = cell,
                         .x = next[0],
                         .y = next[1],
                         .z = next[2],
                         .enteredFace = static_cast<u8>(enteredFace),
                         .directions = static_cast<u8>(directions)});
    };
    // The connectivity bit of every face pair, looked up instead of computed.
    constexpr auto kPairBits = [] {
        std::array<std::array<u16, kChunkFaceCount>, kChunkFaceCount> bits{};
        for (u32 a = 0; a < kChunkFaceCount; ++a) {
            for (u32 b = 0; b < kChunkFaceCount; ++b) {
                bits.at(a).at(b) = a == b ? u16{0} : chunkConnectivityBit(a, b);
            }
        }
        return bits;
    }();

    entered[start] = 0x3F;
    reached = 1;
    const Step first{.cell = static_cast<u32>(start),
                     .x = viewChunk.x - origin.x,
                     .y = viewChunk.y - origin.y,
                     .z = viewChunk.z - origin.z,
                     .enteredFace = 0,
                     .directions = 0};
    for (u32 face = 0; face < kChunkFaceCount; ++face) {
        step(first, face, 1U << face);
    }
    // An index, not an iterator: the queue grows while it is walked.
    for (usize head = 0; head < queue.size(); ++head) { // NOLINT(modernize-loop-convert)
        const Step current = queue[head];
        const u16 cellConnectivity = connectivity[current.cell];
        const std::array<u16, kChunkFaceCount>& pairs = kPairBits.at(current.enteredFace);
        for (u32 face = 0; face < kChunkFaceCount; ++face) {
            // Never back into the entry face (its pair bit is 0), never
            // against a direction the path already moved in, and only
            // through connected faces.
            if ((cellConnectivity & pairs.at(face)) == 0 || (current.directions & (1U << (face ^ 1U))) != 0) {
                continue;
            }
            step(current, face, current.directions | (1U << face));
        }
    }
    searchMs = stopwatch.elapsedMilliseconds();
}

} // namespace vxe::render
