#include "vxe/render/chunk_culling.hpp"

#include <vxe/core/profile.hpp>
#include <vxe/core/time.hpp>

#include <algorithm>

namespace vxe::render {

void CaveCuller::setChunk(ChunkPos position, u16 connectivity) {
    if (connectivity == kChunkAllConnected) {
        removeChunk(position);
        return;
    }
    const auto [it, inserted] = m_chunks.try_emplace(position, connectivity);
    if (inserted || it->second != connectivity) {
        it->second = connectivity;
        m_dirty = true;
    }
}

void CaveCuller::removeChunk(ChunkPos position) {
    if (m_chunks.erase(position) > 0) {
        m_dirty = true;
    }
}

bool CaveCuller::update(ChunkPos camera) {
    if (m_searched && !m_dirty && camera == m_camera) {
        return false;
    }
    search(camera);
    return true;
}

bool CaveCuller::cellIndex(ChunkPos position, usize& index) const noexcept {
    const i64 x = i64{position.x} - m_origin.x;
    const i64 y = i64{position.y} - m_origin.y;
    const i64 z = i64{position.z} - m_origin.z;
    if (x < 0 || y < 0 || z < 0 || x >= m_size[0] || y >= m_size[1] || z >= m_size[2]) {
        return false;
    }
    index = static_cast<usize>(x + (m_size[0] * (y + (i64{m_size[1]} * z))));
    return true;
}

bool CaveCuller::isVisible(ChunkPos position) const noexcept {
    usize index = 0;
    if (!m_searched || m_gaveUp || !cellIndex(position, index)) {
        return true;
    }
    return m_entered[index] != 0;
}

// The box (every known chunk and the camera, plus one chunk of margin) and
// the connectivity of each of its cells. False when the culler gives up.
bool CaveCuller::buildGrid(ChunkPos camera) {
    std::array<i64, 3> low{camera.x, camera.y, camera.z};
    std::array<i64, 3> high = low;
    for (const auto& entry : m_chunks) {
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
    m_gaveUp = m_chunks.empty() || cells > kMaxCells;
    if (m_gaveUp) {
        return false;
    }
    m_origin = {.x = static_cast<i32>(low[0]), .y = static_cast<i32>(low[1]), .z = static_cast<i32>(low[2])};
    for (usize axis = 0; axis < 3; ++axis) {
        m_size.at(axis) = static_cast<i32>(high.at(axis) - low.at(axis) + 1);
    }
    m_connectivity.assign(static_cast<usize>(cells), kChunkAllConnected);
    m_entered.assign(static_cast<usize>(cells), 0);
    for (const auto& [position, connectivity] : m_chunks) {
        usize index = 0;
        if (cellIndex(position, index)) {
            m_connectivity[index] = connectivity;
        }
    }
    return true;
}

void CaveCuller::search(ChunkPos camera) {
    VXE_PROFILE_FUNCTION();
    const Stopwatch stopwatch;
    m_searched = true;
    m_camera = camera;
    ++m_generation;
    usize start = 0;
    // When only the camera moved within the box, the grid still holds.
    if (m_dirty || m_gaveUp || !cellIndex(camera, start)) {
        m_dirty = false;
        if (!buildGrid(camera)) {
            m_reached = 0;
            m_searchMs = stopwatch.elapsedMilliseconds();
            return;
        }
        static_cast<void>(cellIndex(camera, start));
    }
    std::ranges::fill(m_entered, u8{0});

    // Steps between cells: +X, -X, +Y, -Y, +Z, -Z. Each queued step carries
    // its cell coordinates, so stepping needs no divisions.
    constexpr std::array<std::array<i32, 3>, kChunkFaceCount> kOffsets = {
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    const std::array<i64, kChunkFaceCount> stride = {
        1, -1, m_size[0], -m_size[0], i64{m_size[0]} * m_size[1], -(i64{m_size[0]} * m_size[1])};
    m_queue.clear();
    m_reached = 0;
    // Moves from `from` through `face` into the neighbor cell, unless that
    // leaves the box or the neighbor was already entered through that side.
    const auto step = [this, &kOffsets, &stride](const Step& from, u32 face, u32 directions) {
        const std::array<i32, 3>& offset = kOffsets.at(face);
        const std::array<i32, 3> next = {from.x + offset[0], from.y + offset[1], from.z + offset[2]};
        for (usize axis = 0; axis < 3; ++axis) {
            if (next.at(axis) < 0 || next.at(axis) >= m_size.at(axis)) {
                return;
            }
        }
        const auto cell = static_cast<u32>(static_cast<i64>(from.cell) + stride.at(face));
        const u32 enteredFace = face ^ 1U;
        u8& entered = m_entered[cell];
        const auto bit = static_cast<u8>(1U << enteredFace);
        const u16 connectivity = m_connectivity[cell];
        // A path never reverses along an axis, so the directions it moved in
        // are fixed by the cell's offset from the camera. A fully open cell
        // therefore expands the same exits whichever side it was entered
        // from: only the first entry matters. Only partly open cells, where
        // the entry face decides the exits, are searched once per side.
        if ((entered & bit) != 0 || (entered != 0 && connectivity == kChunkAllConnected)) {
            return;
        }
        if (entered == 0) {
            ++m_reached;
        }
        entered = static_cast<u8>(entered | bit);
        if (connectivity == 0) {
            return; // Seen, but nothing is seen through it.
        }
        m_queue.push_back({.cell = cell,
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

    m_entered[start] = 0x3F;
    m_reached = 1;
    const Step origin{.cell = static_cast<u32>(start),
                      .x = camera.x - m_origin.x,
                      .y = camera.y - m_origin.y,
                      .z = camera.z - m_origin.z,
                      .enteredFace = 0,
                      .directions = 0};
    for (u32 face = 0; face < kChunkFaceCount; ++face) {
        step(origin, face, 1U << face);
    }
    // An index, not an iterator: the queue grows while it is walked.
    for (usize head = 0; head < m_queue.size(); ++head) { // NOLINT(modernize-loop-convert)
        const Step current = m_queue[head];
        const u16 connectivity = m_connectivity[current.cell];
        const std::array<u16, kChunkFaceCount>& pairs = kPairBits.at(current.enteredFace);
        for (u32 face = 0; face < kChunkFaceCount; ++face) {
            // Never back into the entry face (its pair bit is 0), never
            // against a direction the path already moved in, and only
            // through connected faces.
            if ((connectivity & pairs.at(face)) == 0 || (current.directions & (1U << (face ^ 1U))) != 0) {
                continue;
            }
            step(current, face, current.directions | (1U << face));
        }
    }
    m_searchMs = stopwatch.elapsedMilliseconds();
}

} // namespace vxe::render
