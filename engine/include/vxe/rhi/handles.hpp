#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>

#include <compare>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// Typed resource handles of the Render Hardware Interface (Section 7.4).
namespace vxe::rhi {

// A typed handle. `index` points into a table inside the backend and
// `generation` detects use after destroy: when a table slot is reused, its
// generation changes, so an old handle no longer matches. The Tag type only
// exists so that a BufferHandle cannot be passed where a TextureHandle is
// expected; it costs nothing at runtime.
template <typename Tag>
struct Handle {
    static constexpr u32 kInvalidIndex = 0xFFFFFF;

    u32 index : 24 = kInvalidIndex;
    u32 generation : 8 = 0;

    // Builds a handle. The mask lets compilers see that the index fits the
    // 24 bit field (GCC warns about the narrowing otherwise); callers already
    // keep indices below kInvalidIndex.
    [[nodiscard]] static constexpr Handle make(u32 slotIndex, u8 slotGeneration) noexcept {
        Handle handle;
        handle.index = slotIndex & kInvalidIndex;
        handle.generation = slotGeneration;
        return handle;
    }

    [[nodiscard]] constexpr bool isValid() const noexcept { return index != kInvalidIndex; }
    friend constexpr bool operator==(const Handle&, const Handle&) = default;

    // The handle as one 32 bit number, for hashing and debug output.
    // Cast first: an 8 bit bitfield promotes to int, and 255 << 24 does not fit.
    [[nodiscard]] constexpr u32 packed() const noexcept {
        return (static_cast<u32>(generation) << 24U) | index;
    }
};

using BufferHandle = Handle<struct BufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using SamplerHandle = Handle<struct SamplerTag>;
using ShaderHandle = Handle<struct ShaderTag>;
using PipelineHandle = Handle<struct PipelineTag>;
using FenceHandle = Handle<struct FenceTag>;
using QueryHandle = Handle<struct QueryTag>;

// A table of backend objects addressed by Handle<Tag>, used by every backend.
//
// Slots are reused through a free list. Each slot carries a generation that
// changes when its object is released, so get() on a stale handle returns
// null instead of a different object that now lives in the slot. With 8 bit
// generations a slot must be reused 256 times before a stale handle could
// match again, which turns almost every use after destroy into a clean
// failure.
//
// Thread safety: none. Only the main thread calls into a Device (Section 6.4).
template <typename Tag, typename T>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    // Stores `object` and returns its handle.
    [[nodiscard]] HandleType allocate(T object) {
        u32 index = 0;
        if (!m_freeList.empty()) {
            index = m_freeList.back();
            m_freeList.pop_back();
            m_slots[index].object = std::move(object);
        } else {
            VXE_VERIFY(m_slots.size() < HandleType::kInvalidIndex, "Handle pool is full");
            index = static_cast<u32>(m_slots.size());
            m_slots.push_back(Slot{.object = std::move(object), .generation = 0});
        }
        ++m_liveCount;
        return HandleType::make(index, m_slots[index].generation);
    }

    // The object behind a handle, or null if the handle is invalid or stale.
    [[nodiscard]] T* get(HandleType handle) noexcept {
        if (!isLive(handle)) {
            return nullptr;
        }
        return &*m_slots[handle.index].object;
    }
    [[nodiscard]] const T* get(HandleType handle) const noexcept {
        if (!isLive(handle)) {
            return nullptr;
        }
        return &*m_slots[handle.index].object;
    }

    // Removes the object and returns it, so the backend can free the GPU
    // resource. Returns nothing for invalid or stale handles.
    std::optional<T> release(HandleType handle) {
        if (!isLive(handle)) {
            return std::nullopt;
        }
        Slot& slot = m_slots[handle.index];
        std::optional<T> object = std::move(slot.object);
        slot.object.reset();
        slot.generation = static_cast<u8>(slot.generation + 1); // Wraps after 255 on purpose.
        m_freeList.push_back(handle.index);
        --m_liveCount;
        return object;
    }

    [[nodiscard]] bool isLive(HandleType handle) const noexcept {
        return handle.isValid() && handle.index < m_slots.size() &&
               m_slots[handle.index].object.has_value() &&
               m_slots[handle.index].generation == handle.generation;
    }

    [[nodiscard]] usize liveCount() const noexcept { return m_liveCount; }

    // Calls fn(handle, object) for every live object, for leak reports.
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (usize i = 0; i < m_slots.size(); ++i) {
            if (m_slots[i].object.has_value()) {
                fn(HandleType::make(static_cast<u32>(i), m_slots[i].generation), *m_slots[i].object);
            }
        }
    }

private:
    struct Slot {
        std::optional<T> object;
        u8 generation = 0;
    };
    std::vector<Slot> m_slots;
    std::vector<u32> m_freeList;
    usize m_liveCount = 0;
};

} // namespace vxe::rhi

template <typename Tag>
struct std::hash<vxe::rhi::Handle<Tag>> {
    [[nodiscard]] std::size_t operator()(const vxe::rhi::Handle<Tag>& handle) const noexcept {
        return std::hash<vxe::u32>{}(handle.packed());
    }
};
