#include "vxe/core/linear_allocator.hpp"

#include <vxe/core/assert.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace vxe {
namespace {

// The block itself is aligned generously so typical SIMD and cache line
// aligned requests need no padding at the very start.
constexpr usize kBlockAlignment = 64;

} // namespace

LinearAllocator::LinearAllocator(usize capacityBytes, MemoryTag tag)
    : m_memory(static_cast<std::byte*>(memory::allocate(capacityBytes, kBlockAlignment, tag)))
    , m_capacity(capacityBytes)
    , m_tag(tag) {}

LinearAllocator::~LinearAllocator() {
    release();
}

LinearAllocator::LinearAllocator(LinearAllocator&& other) noexcept
    : m_memory(std::exchange(other.m_memory, nullptr))
    , m_capacity(std::exchange(other.m_capacity, 0))
    , m_offset(std::exchange(other.m_offset, 0))
    , m_peak(std::exchange(other.m_peak, 0))
    , m_tag(other.m_tag) {}

LinearAllocator& LinearAllocator::operator=(LinearAllocator&& other) noexcept {
    if (this != &other) {
        release();
        m_memory = std::exchange(other.m_memory, nullptr);
        m_capacity = std::exchange(other.m_capacity, 0);
        m_offset = std::exchange(other.m_offset, 0);
        m_peak = std::exchange(other.m_peak, 0);
        m_tag = other.m_tag;
    }
    return *this;
}

void* LinearAllocator::allocate(usize bytes, usize alignment) noexcept {
    VXE_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0, "Alignment must be a power of two");
    // Align the absolute address, not the offset, so alignments larger than
    // kBlockAlignment also work.
    const auto base = reinterpret_cast<std::uintptr_t>(m_memory);
    const std::uintptr_t current = base + m_offset;
    const std::uintptr_t aligned =
        (current + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    const usize padding = aligned - current;

    // Written as subtractions so huge requests cannot overflow the sum.
    if (padding > remainingBytes() || bytes > remainingBytes() - padding) {
        return nullptr;
    }
    m_offset += padding + bytes;
    m_peak = std::max(m_peak, m_offset);
    return m_memory + (aligned - base);
}

void LinearAllocator::rewind(Marker marker) noexcept {
    VXE_ASSERT(marker <= m_offset, "Rewinding forward is not possible: the marker is newer than the arena");
    m_offset = marker;
}

void LinearAllocator::release() noexcept {
    memory::deallocate(m_memory, m_capacity, kBlockAlignment, m_tag);
    m_memory = nullptr;
    m_capacity = 0;
    m_offset = 0;
}

} // namespace vxe
