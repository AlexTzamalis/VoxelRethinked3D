#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/memory.hpp>
#include <vxe/core/types.hpp>

#include <algorithm>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace vxe {

// Allocates and frees objects of one type in O(1) without touching the heap
// in the steady state.
//
// Objects live in slots on pages of `objectsPerPage` slots each. Free slots
// form a linked list threaded through the slots themselves (an intrusive free
// list), so the pool needs no extra bookkeeping memory. A new page is only
// allocated when every slot is in use, and pages are only returned when the
// pool is destroyed. Pointers stay valid until the object is destroyed, even
// when the pool grows, which a std::vector could not guarantee.
//
// Typical use: the chunk pool of the voxel world (Section 9.5).
//
// Thread safety: none. The owning thread creates and destroys all objects.
template <typename T, MemoryTag Tag = MemoryTag::General>
class PoolAllocator {
public:
    explicit PoolAllocator(usize objectsPerPage = 256)
        : m_objectsPerPage(std::max<usize>(objectsPerPage, 1)) {}

    ~PoolAllocator() {
        // Live objects would leak their destructors. That is a bug in the owner.
        VXE_ASSERT(m_liveCount == 0, "PoolAllocator destroyed while objects are still alive");
        for (Slot* page : m_pages) {
            memory::deallocate(page, m_objectsPerPage * sizeof(Slot), alignof(Slot), Tag);
        }
    }

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) = delete;
    PoolAllocator& operator=(PoolAllocator&&) = delete;

    // Constructs a T in a free slot and returns it. Grows by one page if needed.
    template <typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        if (m_freeList == nullptr) {
            addPage();
        }
        Slot* slot = m_freeList;
        m_freeList = slot->next;
        ++m_liveCount;
        return ::new (static_cast<void*>(slot->storage)) T(std::forward<Args>(args)...);
    }

    // Destroys an object created by this pool and makes its slot reusable.
    void destroy(T* object) noexcept {
        if (object == nullptr) {
            return;
        }
        VXE_ASSERT(owns(object), "Object was not created by this pool");
        object->~T();
        // The slot's storage is the first member, so the object's address is
        // the slot's address.
        auto* slot = reinterpret_cast<Slot*>(object);
        slot->next = m_freeList;
        m_freeList = slot;
        --m_liveCount;
    }

    [[nodiscard]] usize liveCount() const noexcept { return m_liveCount; }
    [[nodiscard]] usize capacity() const noexcept { return m_pages.size() * m_objectsPerPage; }
    [[nodiscard]] usize pageCount() const noexcept { return m_pages.size(); }

    // True if `object` points into one of this pool's pages. Linear in the
    // number of pages, so it is meant for asserts.
    [[nodiscard]] bool owns(const T* object) const noexcept {
        const auto* address = reinterpret_cast<const std::byte*>(object);
        return std::ranges::any_of(m_pages, [&](const Slot* page) {
            const auto* begin = reinterpret_cast<const std::byte*>(page);
            const auto* end = begin + m_objectsPerPage * sizeof(Slot);
            return address >= begin && address < end;
        });
    }

private:
    // A slot holds either a live T or, while free, the pointer to the next
    // free slot. The union makes both share the same bytes.
    union Slot {
        Slot* next;
        alignas(T) std::byte storage[sizeof(T)];
    };

    void addPage() {
        auto* page =
            static_cast<Slot*>(memory::allocate(m_objectsPerPage * sizeof(Slot), alignof(Slot), Tag));
        m_pages.push_back(page);
        // Link the new slots in address order, so consecutive create() calls
        // return neighboring memory, which is friendlier to the cache.
        for (usize i = m_objectsPerPage; i > 0; --i) {
            Slot* slot = page + (i - 1);
            slot->next = m_freeList;
            m_freeList = slot;
        }
    }

    std::vector<Slot*> m_pages;
    Slot* m_freeList = nullptr;
    usize m_objectsPerPage;
    usize m_liveCount = 0;
};

} // namespace vxe
