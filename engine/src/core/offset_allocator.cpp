#include "vxe/core/offset_allocator.hpp"

#include <vxe/core/assert.hpp>

#include <algorithm>
#include <bit>

namespace vxe {
namespace {

constexpr u32 kMantissaBits = 3;
constexpr u32 kMantissaValue = 1U << kMantissaBits;
constexpr u32 kMantissaMask = kMantissaValue - 1;

// The lowest set bit of `mask` at or above `start`, or kNoSpace.
u32 lowestSetBitFrom(u32 mask, u32 start) noexcept {
    const u32 masked = start >= 32 ? 0U : mask & ~((1U << start) - 1U);
    return masked == 0 ? OffsetAllocator::kNoSpace : static_cast<u32>(std::countr_zero(masked));
}

} // namespace

// A size as a small float: sizes below 8 are exact ("denormals"); above,
// the exponent is the position of the highest bit and the mantissa the 3
// bits after it. Rounding up adds one when any lower bit is set; a mantissa
// overflow carries into the exponent, which is still the right bin.
u32 OffsetAllocator::binRoundUp(u32 size) noexcept {
    if (size < kMantissaValue) {
        return size;
    }
    const u32 highestBit = 31U - static_cast<u32>(std::countl_zero(size));
    const u32 mantissaStart = highestBit - kMantissaBits;
    const u32 exponent = mantissaStart + 1;
    u32 mantissa = (size >> mantissaStart) & kMantissaMask;
    if ((size & ((1U << mantissaStart) - 1U)) != 0) {
        ++mantissa;
    }
    return (exponent << kMantissaBits) + mantissa;
}

u32 OffsetAllocator::binRoundDown(u32 size) noexcept {
    if (size < kMantissaValue) {
        return size;
    }
    const u32 highestBit = 31U - static_cast<u32>(std::countl_zero(size));
    const u32 mantissaStart = highestBit - kMantissaBits;
    const u32 exponent = mantissaStart + 1;
    const u32 mantissa = (size >> mantissaStart) & kMantissaMask;
    return (exponent << kMantissaBits) | mantissa;
}

u64 OffsetAllocator::binSize(u32 bin) noexcept {
    if (bin >= kBins) {
        return ~u64{0}; // Not a bin.
    }
    const u32 exponent = bin >> kMantissaBits;
    const u64 mantissa = bin & kMantissaMask;
    return exponent == 0 ? mantissa : (mantissa | kMantissaValue) << (exponent - 1);
}

OffsetAllocator::OffsetAllocator(u32 size, u32 maxAllocations)
    : m_size(size)
    , m_maxAllocations(std::max(maxAllocations, 1U)) {
    // Each allocation can leave a free range beside it: 2n + 1 nodes cover
    // n allocations in any layout.
    const usize nodes = (static_cast<usize>(m_maxAllocations) * 2) + 1;
    m_nodes.resize(nodes);
    m_freeNodes.resize(nodes);
    reset();
}

void OffsetAllocator::reset() noexcept {
    m_freeStorage = 0;
    m_liveAllocations = 0;
    m_usedBinsTop = 0;
    m_usedBins.fill(0);
    m_binHeads.fill(kUnused);
    // Node indices are handed out from the top of the stack: 0 first.
    const auto count = static_cast<u32>(m_nodes.size());
    for (u32 i = 0; i < count; ++i) {
        m_freeNodes[i] = count - 1 - i;
    }
    m_freeNodeCount = count;
    if (m_size > 0) {
        static_cast<void>(insertIntoBin(m_size, 0));
    }
}

u32 OffsetAllocator::insertIntoBin(u32 size, u32 offset) noexcept {
    VXE_ASSERT(m_freeNodeCount > 0, "OffsetAllocator ran out of nodes");
    // Free ranges go into the bin rounded down, so every range in a bin is
    // at least the bin's size.
    const u32 bin = binRoundDown(size);
    const u32 top = bin / kLeafBins;
    const u32 leaf = bin % kLeafBins;
    if (m_binHeads[bin] == kUnused) {
        m_usedBins[top] = static_cast<u8>(m_usedBins[top] | (1U << leaf));
        m_usedBinsTop |= 1U << top;
    }
    const u32 head = m_binHeads[bin];
    const u32 nodeIndex = m_freeNodes[--m_freeNodeCount];
    m_nodes[nodeIndex] = Node{.offset = offset, .size = size, .binNext = head};
    if (head != kUnused) {
        m_nodes[head].binPrevious = nodeIndex;
    }
    m_binHeads[bin] = nodeIndex;
    m_freeStorage += size;
    return nodeIndex;
}

void OffsetAllocator::unlinkFromBin(u32 nodeIndex) noexcept {
    Node& node = m_nodes[nodeIndex];
    if (node.binPrevious != kUnused) {
        m_nodes[node.binPrevious].binNext = node.binNext;
        if (node.binNext != kUnused) {
            m_nodes[node.binNext].binPrevious = node.binPrevious;
        }
    } else {
        // The bin's first range: the bin may become empty.
        const u32 bin = binRoundDown(node.size);
        m_binHeads[bin] = node.binNext;
        if (node.binNext != kUnused) {
            m_nodes[node.binNext].binPrevious = kUnused;
        } else {
            const u32 top = bin / kLeafBins;
            m_usedBins[top] = static_cast<u8>(m_usedBins[top] & ~(1U << (bin % kLeafBins)));
            if (m_usedBins[top] == 0) {
                m_usedBinsTop &= ~(1U << top);
            }
        }
    }
    node.binPrevious = kUnused;
    node.binNext = kUnused;
    m_freeStorage -= node.size;
}

void OffsetAllocator::removeFromBin(u32 nodeIndex) noexcept {
    unlinkFromBin(nodeIndex);
    m_freeNodes[m_freeNodeCount++] = nodeIndex;
}

u32 OffsetAllocator::findFreeRange(u32 size) const noexcept {
    // The smallest bin whose every range is big enough, then the first used
    // bin at or above it: first among its 8 siblings, then in later groups.
    const u32 minBin = binRoundUp(size);
    const u32 minTop = minBin / kLeafBins;
    u32 top = minTop;
    u32 leaf = kNoSpace;
    if (minTop < kTopBins && (m_usedBinsTop & (1U << top)) != 0) {
        leaf = lowestSetBitFrom(m_usedBins[top], minBin % kLeafBins);
    }
    if (leaf == kNoSpace) {
        top = lowestSetBitFrom(m_usedBinsTop, minTop + 1);
    }
    if (top != kNoSpace) {
        if (leaf == kNoSpace) {
            leaf = static_cast<u32>(std::countr_zero(static_cast<u32>(m_usedBins[top])));
        }
        return m_binHeads[(top * kLeafBins) + leaf];
    }
    // Nothing in the bins that surely fit. The bin below holds ranges from
    // its size up to the next bin's size, so one of them may still be big
    // enough (for example a request for exactly all the free space). This
    // walk only runs when the fast path failed.
    for (u32 index = m_binHeads[binRoundDown(size)]; index != kUnused; index = m_nodes[index].binNext) {
        if (m_nodes[index].size >= size) {
            return index;
        }
    }
    return kUnused;
}

OffsetAllocator::Allocation OffsetAllocator::allocate(u32 size) noexcept {
    VXE_ASSERT(size > 0, "Allocations need at least one unit");
    // A split needs a spare node for the rest of the range.
    if (size == 0 || m_freeNodeCount < 2 || m_liveAllocations == m_maxAllocations) {
        return {};
    }
    const u32 nodeIndex = findFreeRange(size);
    if (nodeIndex == kUnused) {
        return {};
    }
    unlinkFromBin(nodeIndex);
    Node& node = m_nodes[nodeIndex];
    const u32 rangeSize = node.size;
    node.size = size;
    node.used = true;
    ++m_liveAllocations;

    // The rest goes back as a free range right after the allocation.
    if (rangeSize > size) {
        const u32 restIndex = insertIntoBin(rangeSize - size, node.offset + size);
        Node& allocated = m_nodes[nodeIndex];
        Node& rest = m_nodes[restIndex];
        if (allocated.neighborNext != kUnused) {
            m_nodes[allocated.neighborNext].neighborPrevious = restIndex;
        }
        rest.neighborPrevious = nodeIndex;
        rest.neighborNext = allocated.neighborNext;
        allocated.neighborNext = restIndex;
    }
    return {.offset = m_nodes[nodeIndex].offset, .node = nodeIndex};
}

void OffsetAllocator::free(Allocation allocation) noexcept {
    if (!allocation.isValid()) {
        return;
    }
    VXE_ASSERT(allocation.node < m_nodes.size() && m_nodes[allocation.node].used,
               "Freeing an allocation that is not live");
    const Node& node = m_nodes[allocation.node];
    u32 offset = node.offset;
    u32 size = node.size;
    u32 neighborPrevious = node.neighborPrevious;
    u32 neighborNext = node.neighborNext;

    // Merge with free neighbors on both sides.
    if (neighborPrevious != kUnused && !m_nodes[neighborPrevious].used) {
        const Node& previous = m_nodes[neighborPrevious];
        offset = previous.offset;
        size += previous.size;
        const u32 before = previous.neighborPrevious;
        removeFromBin(neighborPrevious);
        neighborPrevious = before;
    }
    if (neighborNext != kUnused && !m_nodes[neighborNext].used) {
        const Node& next = m_nodes[neighborNext];
        size += next.size;
        const u32 after = next.neighborNext;
        removeFromBin(neighborNext);
        neighborNext = after;
    }
    m_freeNodes[m_freeNodeCount++] = allocation.node;
    --m_liveAllocations;

    const u32 merged = insertIntoBin(size, offset);
    m_nodes[merged].neighborPrevious = neighborPrevious;
    m_nodes[merged].neighborNext = neighborNext;
    if (neighborPrevious != kUnused) {
        m_nodes[neighborPrevious].neighborNext = merged;
    }
    if (neighborNext != kUnused) {
        m_nodes[neighborNext].neighborPrevious = merged;
    }
}

u32 OffsetAllocator::sizeOf(Allocation allocation) const noexcept {
    return allocation.isValid() ? m_nodes[allocation.node].size : 0;
}

OffsetAllocator::Report OffsetAllocator::report() const noexcept {
    u32 largest = 0;
    if (m_usedBinsTop != 0) {
        const u32 top = 31U - static_cast<u32>(std::countl_zero(m_usedBinsTop));
        const u32 leaf = 31U - static_cast<u32>(std::countl_zero(static_cast<u32>(m_usedBins[top])));
        largest = static_cast<u32>(std::min<u64>(binSize((top * kLeafBins) + leaf), m_size));
    }
    return {.freeSize = m_freeStorage, .largestFree = largest, .allocations = m_liveAllocations};
}

} // namespace vxe
