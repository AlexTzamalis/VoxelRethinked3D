#pragma once

#include <vxe/core/memory.hpp>
#include <vxe/core/types.hpp>

#include <cstddef>
#include <span>
#include <type_traits>

namespace vxe {

// A bump allocator over one fixed block of memory, also called an arena.
//
// Allocating moves an offset forward, which costs a few instructions and never
// touches the heap. Individual allocations are never freed; instead the whole
// arena is reset at once (for example at the start of every frame), or rewound
// to a marker taken earlier. This is what makes "no heap allocations in per
// frame hot loops" (Section 0.2) practical.
//
// Because nothing is freed individually, destructors never run. Only types
// that are trivially destructible may be placed in it, which the typed helpers
// enforce at compile time.
//
// Thread safety: none. Each thread uses its own arena (see ScratchScope).
class LinearAllocator {
public:
    // A position in the arena that rewind() can return to.
    using Marker = usize;

    // Reserves `capacityBytes` up front, recorded under `tag`.
    explicit LinearAllocator(usize capacityBytes, MemoryTag tag = MemoryTag::General);
    ~LinearAllocator();

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;
    LinearAllocator(LinearAllocator&& other) noexcept;
    LinearAllocator& operator=(LinearAllocator&& other) noexcept;

    // Returns `bytes` of memory aligned to `alignment` (a power of two), or
    // nullptr if the arena is full. Callers decide whether full is an error.
    [[nodiscard]] void* allocate(usize bytes, usize alignment = alignof(std::max_align_t)) noexcept;

    // Returns uninitialized storage for `count` objects of type T, or an empty
    // span if the arena is full. T must be trivial so the bytes can be used as
    // objects right away and nothing needs destroying later.
    template <typename T>
    [[nodiscard]] std::span<T> allocateArray(usize count) noexcept {
        static_assert(std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>,
                      "LinearAllocator only holds trivial types: their destructors would never run");
        if (count > m_capacity / sizeof(T)) {
            return {};
        }
        void* memory = allocate(count * sizeof(T), alignof(T));
        return memory == nullptr ? std::span<T>{} : std::span<T>(static_cast<T*>(memory), count);
    }

    [[nodiscard]] Marker marker() const noexcept { return m_offset; }

    // Frees everything allocated after `marker` was taken.
    void rewind(Marker marker) noexcept;

    // Frees everything. The memory block itself stays reserved.
    void reset() noexcept { m_offset = 0; }

    [[nodiscard]] usize usedBytes() const noexcept { return m_offset; }
    [[nodiscard]] usize capacityBytes() const noexcept { return m_capacity; }
    [[nodiscard]] usize remainingBytes() const noexcept { return m_capacity - m_offset; }
    // The highest usedBytes() ever reached. Useful to size budgets from real data.
    [[nodiscard]] usize peakBytes() const noexcept { return m_peak; }

private:
    void release() noexcept;

    std::byte* m_memory = nullptr;
    usize m_capacity = 0;
    usize m_offset = 0;
    usize m_peak = 0;
    MemoryTag m_tag = MemoryTag::General;
};

} // namespace vxe
