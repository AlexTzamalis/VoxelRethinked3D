#pragma once

#include <vxe/core/linear_allocator.hpp>
#include <vxe/core/types.hpp>

#include <cstddef>
#include <span>

namespace vxe {

// Size of each thread's scratch arena. Large enough for every hot loop the
// plan describes (a padded 18^3 chunk snapshot is 11.4 KB, a full mesh output
// well under 1 MB), small enough that 16 worker threads cost only 64 MB.
inline constexpr usize kScratchBytesPerThread = 4 * 1024 * 1024;

// Temporary memory for one function call on the current thread.
//
// Each thread has its own scratch arena (a LinearAllocator, created on first
// use). A ScratchScope remembers the arena position when it is created and
// rewinds to it when it is destroyed, so everything allocated through the
// scope is released at once, without any heap traffic:
//
//   void meshChunk(...) {
//       ScratchScope scratch;
//       std::span<u16> blocks = scratch.allocateArray<u16>(18 * 18 * 18);
//       ...
//   } // blocks released here
//
// Scopes nest like the call stack: an inner scope must end before an outer
// one, which happens naturally when scopes are local variables.
//
// Thread safety: a scope belongs to the thread that created it and must not
// be handed to another thread.
class ScratchScope {
public:
    ScratchScope() noexcept;
    ~ScratchScope();

    ScratchScope(const ScratchScope&) = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
    ScratchScope(ScratchScope&&) = delete;
    ScratchScope& operator=(ScratchScope&&) = delete;

    // Same contract as LinearAllocator::allocate: nullptr when the arena is full.
    [[nodiscard]] void* allocate(usize bytes, usize alignment = alignof(std::max_align_t)) noexcept {
        return m_arena.allocate(bytes, alignment);
    }

    // Same contract as LinearAllocator::allocateArray: empty when the arena is full.
    template <typename T>
    [[nodiscard]] std::span<T> allocateArray(usize count) noexcept {
        return m_arena.allocateArray<T>(count);
    }

    [[nodiscard]] usize remainingBytes() const noexcept { return m_arena.remainingBytes(); }

    // The calling thread's arena, for diagnostics (peak use per thread).
    [[nodiscard]] static const LinearAllocator& threadArena() noexcept;

private:
    LinearAllocator& m_arena;
    LinearAllocator::Marker m_marker;
};

} // namespace vxe
