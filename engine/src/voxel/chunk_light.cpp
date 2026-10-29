#include "vxe/voxel/chunk_light.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/core/memory.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace vxe::voxel {
namespace {

u8* allocateLight() {
    return static_cast<u8*>(memory::allocate(kChunkVolume, alignof(u64), MemoryTag::Chunks));
}

void freeLight(u8* data) noexcept {
    memory::deallocate(data, kChunkVolume, alignof(u64), MemoryTag::Chunks);
}

} // namespace

ChunkLight::~ChunkLight() {
    release();
}

ChunkLight::ChunkLight(const ChunkLight& other)
    : m_uniform(other.m_uniform) {
    if (other.m_data != nullptr) {
        m_data = allocateLight();
        std::memcpy(m_data, other.m_data, kChunkVolume);
    }
}

ChunkLight& ChunkLight::operator=(const ChunkLight& other) {
    if (this != &other) {
        ChunkLight copy(other);
        *this = std::move(copy);
    }
    return *this;
}

ChunkLight::ChunkLight(ChunkLight&& other) noexcept
    : m_data(std::exchange(other.m_data, nullptr))
    , m_uniform(std::exchange(other.m_uniform, kOpenSkyLight)) {}

ChunkLight& ChunkLight::operator=(ChunkLight&& other) noexcept {
    if (this != &other) {
        release();
        m_data = std::exchange(other.m_data, nullptr);
        m_uniform = std::exchange(other.m_uniform, kOpenSkyLight);
    }
    return *this;
}

void ChunkLight::release() noexcept {
    if (m_data != nullptr) {
        freeLight(m_data);
        m_data = nullptr;
    }
}

void ChunkLight::setIndex(u16 index, u8 packed) {
    VXE_ASSERT(index < kChunkVolume, "Block index out of range");
    if (m_data == nullptr) {
        if (packed == m_uniform) {
            return;
        }
        m_data = allocateLight();
        std::memset(m_data, m_uniform, kChunkVolume);
    }
    m_data[index] = packed;
}

void ChunkLight::fill(u8 packed) noexcept {
    release();
    m_uniform = packed;
}

void ChunkLight::assign(std::span<const u8, kChunkVolume> packed) {
    const u8 first = packed[0];
    if (std::ranges::all_of(packed, [first](u8 value) { return value == first; })) {
        fill(first);
        return;
    }
    if (m_data == nullptr) {
        m_data = allocateLight();
    }
    std::memcpy(m_data, packed.data(), kChunkVolume);
}

bool ChunkLight::compact() noexcept {
    if (m_data == nullptr) {
        return false;
    }
    const std::span<const u8, kChunkVolume> values(m_data, kChunkVolume);
    const u8 first = values[0];
    if (!std::ranges::all_of(values, [first](u8 value) { return value == first; })) {
        return false;
    }
    fill(first);
    return true;
}

void ChunkLight::copyTo(std::span<u8> out, usize rowStride, usize sliceStride) const noexcept {
    VXE_ASSERT(out.size() >= (15 * sliceStride) + (15 * rowStride) + 16, "Light copy target too small");
    for (usize y = 0; y < 16; ++y) {
        for (usize z = 0; z < 16; ++z) {
            u8* row = out.data() + (y * sliceStride) + (z * rowStride);
            if (m_data == nullptr) {
                std::memset(row, m_uniform, 16);
            } else {
                std::memcpy(row, m_data + (y * 256) + (z * 16), 16);
            }
        }
    }
}

bool ChunkLight::equals(const ChunkLight& other) const noexcept {
    if (m_data == nullptr && other.m_data == nullptr) {
        return m_uniform == other.m_uniform;
    }
    if (m_data != nullptr && other.m_data != nullptr) {
        return std::memcmp(m_data, other.m_data, kChunkVolume) == 0;
    }
    const ChunkLight& full = m_data != nullptr ? *this : other;
    const u8 value = m_data != nullptr ? other.m_uniform : m_uniform;
    const std::span<const u8, kChunkVolume> values(full.m_data, kChunkVolume);
    return std::ranges::all_of(values, [value](u8 entry) { return entry == value; });
}

} // namespace vxe::voxel
