#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>

#include <cstddef>
#include <limits>
#include <string_view>

// Memory accounting by tag.
//
// Every engine allocation that matters for budgets goes through
// vxe::memory::allocate() with a MemoryTag, directly or through one of the
// allocators built on it (TrackingAllocator, LinearAllocator, PoolAllocator).
// The per tag totals feed the Memory page of the debug overlay (Section 18.2).
//
// Thread safety: all functions in this header may be called from any thread.
// The counters are relaxed atomics, so totals read while other threads
// allocate are a recent snapshot, not an exact instant.
namespace vxe {

enum class MemoryTag : u8 {
    General,
    Chunks,   // Block and light storage of chunks.
    Meshes,   // CPU side mesh output before upload.
    Staging,  // CPU copies of GPU uploads.
    Lod,      // Far LOD tiles and their caches.
    Textures, // Decoded images and texture pack data.
    Ui,
    Jobs,    // Job system queues and job payloads.
    Scratch, // Thread local scratch arenas.
    Debug,   // Overlay history, stats and captures.
    Count,   // Number of tags, not a real tag.
};

inline constexpr usize kMemoryTagCount = static_cast<usize>(MemoryTag::Count);

namespace memory {

// A snapshot of one tag's counters.
struct TagStats {
    usize currentBytes = 0; // Bytes allocated and not yet freed.
    usize peakBytes = 0;    // Highest currentBytes seen since startup.
    u64 allocations = 0;    // Number of allocations since startup.
    u64 frees = 0;          // Number of frees since startup.
};

// Returns the name of a tag, for example "Chunks". Not called toString because
// doctest uses that name through ADL to print values.
[[nodiscard]] std::string_view nameOf(MemoryTag tag) noexcept;

[[nodiscard]] TagStats stats(MemoryTag tag) noexcept;

// Sum of currentBytes over all tags.
[[nodiscard]] usize totalBytes() noexcept;

// Allocates `bytes` with the given alignment (a power of two) and records it
// under `tag`. Never returns nullptr: running out of memory is fatal and
// stops the program through VXE_VERIFY.
[[nodiscard]] void* allocate(usize bytes, usize alignment, MemoryTag tag);

// Frees memory from allocate(). `bytes`, `alignment` and `tag` must be the
// values used to allocate it, which every caller knows, so no size header is
// stored in front of each block. nullptr is ignored.
void deallocate(void* pointer, usize bytes, usize alignment, MemoryTag tag) noexcept;

// Records an allocation or free made elsewhere (for example GPU memory or a
// third party library) so it still shows up in the totals.
void recordAllocation(MemoryTag tag, usize bytes) noexcept;
void recordFree(MemoryTag tag, usize bytes) noexcept;

// Every call to the global operator new since startup, from any thread. The
// debug overlay shows the difference per frame, which should be near zero
// while playing (Section 18.2, Memory page). The count comes from replacing
// the global operator new in core; builds with sanitizers keep their own
// allocator instead, and there the count is not available.
[[nodiscard]] u64 globalAllocationCount() noexcept;
[[nodiscard]] bool globalAllocationCountAvailable() noexcept;

} // namespace memory

// A standard library allocator that records its memory under a tag, so
// containers show up in the per tag totals:
//   std::vector<u16, TrackingAllocator<u16, MemoryTag::Chunks>> palette;
// It is stateless, so any two instances compare equal and containers can
// swap and move memory freely.
template <typename T, MemoryTag Tag>
class TrackingAllocator {
public:
    using value_type = T; // NOLINT(readability-identifier-naming): required by the standard.

    TrackingAllocator() noexcept = default;
    template <typename U>
    // NOLINTNEXTLINE(google-explicit-constructor, hicpp-explicit-conversions): containers convert implicitly.
    TrackingAllocator(const TrackingAllocator<U, Tag>& /*other*/) noexcept {}

    // The standard can only rebind allocators with type parameters on its own,
    // so the tag parameter needs this explicit rebind.
    template <typename U>
    struct rebind { // NOLINT(readability-identifier-naming): required by the standard.
        using other = TrackingAllocator<U, Tag>; // NOLINT(readability-identifier-naming)
    };

    [[nodiscard]] T* allocate(usize count) {
        constexpr usize kMaxCount = std::numeric_limits<usize>::max() / sizeof(T);
        VXE_VERIFY(count <= kMaxCount, "TrackingAllocator size overflow");
        return static_cast<T*>(memory::allocate(count * sizeof(T), alignof(T), Tag));
    }

    void deallocate(T* pointer, usize count) noexcept {
        memory::deallocate(pointer, count * sizeof(T), alignof(T), Tag);
    }

    template <typename U>
    friend bool operator==(const TrackingAllocator& /*lhs*/,
                           const TrackingAllocator<U, Tag>& /*rhs*/) noexcept {
        return true;
    }
};

} // namespace vxe
