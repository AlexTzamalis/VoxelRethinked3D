#include "vxe/core/scratch.hpp"

namespace vxe {
namespace {

LinearAllocator& arenaOfThisThread() noexcept {
    // Created on first use, so threads that never need scratch memory never
    // pay for it, and destroyed when the thread exits.
    thread_local LinearAllocator arena(kScratchBytesPerThread, MemoryTag::Scratch);
    return arena;
}

} // namespace

ScratchScope::ScratchScope() noexcept
    : m_arena(arenaOfThisThread())
    , m_marker(m_arena.marker()) {}

ScratchScope::~ScratchScope() {
    m_arena.rewind(m_marker);
}

const LinearAllocator& ScratchScope::threadArena() noexcept {
    return arenaOfThisThread();
}

} // namespace vxe
