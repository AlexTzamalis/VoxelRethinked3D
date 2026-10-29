#pragma once

#include <vxe/core/types.hpp>

#include <array>
#include <vector>

namespace vxe {

// Hands out ranges of a large linear space (offsets, not memory) in O(1):
// the chunk vertex arena on the GPU (Section 8.2) allocates its quads here.
// The allocator never touches the memory it manages, so it works for GPU
// buffers the CPU cannot read.
//
// The design follows Sebastian Aaltonen's OffsetAllocator (the library the
// plan names; vcpkg has no port, so the engine carries its own version):
//
// - Free ranges are sorted into 256 bins by size. A bin index is the size
//   written as a tiny float, 5 exponent and 3 mantissa bits, so bins are
//   exact for sizes below 8 and then about 12% apart up to 4 billion.
// - Two bit masks (one bit per group of 8 bins, then one per bin) say
//   which bins have free ranges, so finding the smallest bin that fits is
//   two count trailing zeros instead of a search.
// - Allocating takes the first range of that bin and puts the rest of it
//   back as a new free range. Freeing merges a range with free neighbors
//   (ranges are linked in address order), so free space does not stay
//   split up.
//
// Sizes are rounded up to the next bin when searching, which is what makes
// the search O(1). Only when that finds nothing does allocate() walk the
// one bin below, whose ranges may still be big enough, so a request for
// exactly the free space left still succeeds.
//
// All bookkeeping is preallocated (a node per possible range), so neither
// allocate() nor free() touches the heap.
//
// Thread safety: none; one owner.
class OffsetAllocator {
public:
    static constexpr u32 kNoSpace = 0xFFFFFFFFU;

    struct Allocation {
        u32 offset = kNoSpace;
        u32 node = kNoSpace; // Internal: which bookkeeping node holds it.

        [[nodiscard]] bool isValid() const noexcept { return offset != kNoSpace; }
    };

    struct Report {
        u32 freeSize = 0;    // Total free units.
        u32 largestFree = 0; // A lower bound of the largest free range (its bin size).
        u32 allocations = 0; // Live allocations.
    };

    // Manages offsets 0 to size - 1. maxAllocations bounds the number of
    // live allocations; free ranges need nodes too, so the pool holds
    // 2 * maxAllocations + 1 nodes (28 bytes each).
    explicit OffsetAllocator(u32 size, u32 maxAllocations = 65536);

    // A range of `size` units, or an invalid allocation if nothing fits or
    // all nodes are in use. size must be at least 1.
    [[nodiscard]] Allocation allocate(u32 size) noexcept;
    // Returns an allocation. Invalid allocations are ignored.
    void free(Allocation allocation) noexcept;
    // The size an allocation was made with.
    [[nodiscard]] u32 sizeOf(Allocation allocation) const noexcept;

    // Frees everything.
    void reset() noexcept;

    [[nodiscard]] u32 capacity() const noexcept { return m_size; }
    [[nodiscard]] u32 freeSize() const noexcept { return m_freeStorage; }
    [[nodiscard]] Report report() const noexcept;

    // Small float bin helpers, public for tests.
    [[nodiscard]] static u32 binRoundUp(u32 size) noexcept;
    [[nodiscard]] static u32 binRoundDown(u32 size) noexcept;
    // The smallest size of a bin; 64 bit, because the top bins stand for
    // sizes beyond 32 bits.
    [[nodiscard]] static u64 binSize(u32 bin) noexcept;

private:
    static constexpr u32 kUnused = 0xFFFFFFFFU;
    static constexpr u32 kTopBins = 32;
    static constexpr u32 kLeafBins = 8;
    static constexpr u32 kBins = kTopBins * kLeafBins;

    struct Node {
        u32 offset = 0;
        u32 size = 0;
        u32 binPrevious = kUnused; // Free ranges in the same bin.
        u32 binNext = kUnused;
        u32 neighborPrevious = kUnused; // Ranges next to this one in address order.
        u32 neighborNext = kUnused;
        bool used = false;
    };

    [[nodiscard]] u32 insertIntoBin(u32 size, u32 offset) noexcept;
    void unlinkFromBin(u32 nodeIndex) noexcept;
    void removeFromBin(u32 nodeIndex) noexcept;
    [[nodiscard]] u32 findFreeRange(u32 size) const noexcept;

    u32 m_size;
    u32 m_maxAllocations;
    u32 m_freeStorage = 0;
    u32 m_liveAllocations = 0;
    u32 m_usedBinsTop = 0;
    std::array<u8, kTopBins> m_usedBins{};
    std::array<u32, kBins> m_binHeads{};
    std::vector<Node> m_nodes;
    std::vector<u32> m_freeNodes; // A stack of unused node indices.
    u32 m_freeNodeCount = 0;
};

} // namespace vxe
