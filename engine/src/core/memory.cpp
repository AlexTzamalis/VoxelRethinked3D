#include "vxe/core/memory.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

namespace vxe::memory {
namespace {

// Counters of one tag. Each tag sits on its own cache line so threads that
// allocate under different tags (meshing versus texture loading) never fight
// over the same line. The padding this adds is the point, so the MSVC warning
// about it (C4324) is silenced for this struct only.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(64) TagCounters {
    std::atomic<usize> currentBytes{0};
    std::atomic<usize> peakBytes{0};
    std::atomic<u64> allocations{0};
    std::atomic<u64> frees{0};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

std::array<TagCounters, kMemoryTagCount>& counters() noexcept {
    // A function local static is initialized on first use, so allocations made
    // during static initialization of other files are counted correctly.
    static std::array<TagCounters, kMemoryTagCount> instance;
    return instance;
}

TagCounters& countersOf(MemoryTag tag) noexcept {
    VXE_ASSERT(tag < MemoryTag::Count, "Invalid memory tag");
    return counters()[static_cast<usize>(tag)];
}

} // namespace

std::string_view nameOf(MemoryTag tag) noexcept {
    switch (tag) {
    case MemoryTag::General:
        return "General";
    case MemoryTag::Chunks:
        return "Chunks";
    case MemoryTag::Meshes:
        return "Meshes";
    case MemoryTag::Staging:
        return "Staging";
    case MemoryTag::Lod:
        return "Lod";
    case MemoryTag::Textures:
        return "Textures";
    case MemoryTag::Ui:
        return "Ui";
    case MemoryTag::Jobs:
        return "Jobs";
    case MemoryTag::Scratch:
        return "Scratch";
    case MemoryTag::Debug:
        return "Debug";
    case MemoryTag::Count:
        break;
    }
    return "Invalid";
}

TagStats stats(MemoryTag tag) noexcept {
    const TagCounters& c = countersOf(tag);
    return TagStats{
        .currentBytes = c.currentBytes.load(std::memory_order_relaxed),
        .peakBytes = c.peakBytes.load(std::memory_order_relaxed),
        .allocations = c.allocations.load(std::memory_order_relaxed),
        .frees = c.frees.load(std::memory_order_relaxed),
    };
}

usize totalBytes() noexcept {
    usize total = 0;
    for (const TagCounters& c : counters()) {
        total += c.currentBytes.load(std::memory_order_relaxed);
    }
    return total;
}

void recordAllocation(MemoryTag tag, usize bytes) noexcept {
    TagCounters& c = countersOf(tag);
    c.allocations.fetch_add(1, std::memory_order_relaxed);
    const usize now = c.currentBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    // Raise the peak if we passed it. The loop only repeats when another
    // thread raised the peak at the same moment.
    usize peak = c.peakBytes.load(std::memory_order_relaxed);
    while (now > peak && !c.peakBytes.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
}

void recordFree(MemoryTag tag, usize bytes) noexcept {
    TagCounters& c = countersOf(tag);
    c.frees.fetch_add(1, std::memory_order_relaxed);
    c.currentBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

void* allocate(usize bytes, usize alignment, MemoryTag tag) {
    VXE_ASSERT(alignment != 0 && (alignment & (alignment - 1)) == 0, "Alignment must be a power of two");
    // The nothrow form returns nullptr instead of throwing std::bad_alloc,
    // because engine code does not use exceptions (Section 23.4).
    void* pointer = ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
    VXE_VERIFY(pointer != nullptr, "Out of memory");
    recordAllocation(tag, bytes);
    return pointer;
}

void deallocate(void* pointer, usize bytes, usize alignment, MemoryTag tag) noexcept {
    if (pointer == nullptr) {
        return;
    }
    ::operator delete(pointer, std::align_val_t{alignment});
    recordFree(tag, bytes);
}

} // namespace vxe::memory

// Global allocation counting (Memory page of the debug overlay).
//
// Replacing the global operator new and delete is the only way to see every
// allocation, including those inside the standard library. They forward to
// malloc and free, exactly like the defaults, and only add one relaxed atomic
// increment. They live in this file because every program on the engine links
// it. Sanitizer builds skip the replacement: ASan and TSan install their own
// operator new, and two definitions would clash.
namespace {

// constinit: ready before any static initializer allocates.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): a process wide counter by nature.
constinit std::atomic<vxe::u64> globalAllocations{0};

} // namespace

vxe::u64 vxe::memory::globalAllocationCount() noexcept {
    return globalAllocations.load(std::memory_order_relaxed);
}

#ifdef VXE_SANITIZER_ACTIVE

bool vxe::memory::globalAllocationCountAvailable() noexcept {
    return false;
}

#else

bool vxe::memory::globalAllocationCountAvailable() noexcept {
    return true;
}

// The replaceable allocation functions, with the standard's behavior: never
// return null, retry through the new handler, throw std::bad_alloc when there
// is none. Array and nothrow forms are implemented by the library on top of
// these on every supported compiler, so they are counted too.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory, misc-new-delete-overloads)
void* operator new(std::size_t size) {
    globalAllocations.fetch_add(1, std::memory_order_relaxed);
    const std::size_t bytes = size == 0 ? 1 : size;
    while (true) {
        if (void* pointer = std::malloc(bytes)) {
            return pointer;
        }
        std::new_handler handler =
            std::get_new_handler(); // NOLINT(misc-const-correctness): a function pointer alias.
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

// The pointer parameter is spelled like MSVC's own declarations
// (vcruntime_new.h), because clang-tidy compares definition and declaration
// names; the reserved spelling is harmless here.
// NOLINTBEGIN(bugprone-reserved-identifier, readability-identifier-naming)
void operator delete(void* _Block) noexcept {
    std::free(_Block);
}

void operator delete[](void* _Block) noexcept {
    std::free(_Block);
}

void operator delete(void* _Block, std::size_t /*size*/) noexcept {
    std::free(_Block);
}

void operator delete[](void* _Block, std::size_t /*size*/) noexcept {
    std::free(_Block);
}
// NOLINTEND(bugprone-reserved-identifier, readability-identifier-naming)
// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory, misc-new-delete-overloads)

#endif
