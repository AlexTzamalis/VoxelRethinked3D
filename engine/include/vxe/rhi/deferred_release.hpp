#pragma once

#include <vxe/core/types.hpp>

#include <deque>
#include <utility>

namespace vxe::rhi {

// Holds backend objects that were destroyed through the RHI while the GPU may
// still be using them, until the frame that last could use them has finished
// (Section 7.5, E6.6).
//
// Destroying a handle makes it invalid right away, but the GPU usually runs a
// frame or two behind the CPU, so commands that use the object may still be
// in flight. A backend therefore pushes the object here, tagged with the
// serial number of the frame being recorded, and frees it once the GPU has
// completed that frame (known from a fence the backend inserts per frame).
// OpenGL would survive immediate deletion, but Vulkan does not, so the rule
// lives in the shared RHI layer from the start.
//
// Serials only grow, so items leave in the order they arrived.
//
// Thread safety: main thread only, like the device that owns it.
template <typename T>
class DeferredReleaseQueue {
public:
    // Queues `item`, which the GPU may use until frame `frameSerial` completes.
    void push(T item, u64 frameSerial) { m_entries.push_back({std::move(item), frameSerial}); }

    // Calls release(item) for every item whose frame is complete, oldest
    // first, and returns how many were released.
    template <typename Release>
    usize releaseCompleted(u64 completedSerial, Release&& release) {
        usize released = 0;
        while (!m_entries.empty() && m_entries.front().frameSerial <= completedSerial) {
            release(m_entries.front().item);
            m_entries.pop_front();
            ++released;
        }
        return released;
    }

    // Releases everything, for when the GPU is known to be idle.
    template <typename Release>
    usize releaseAll(Release&& release) {
        const usize released = m_entries.size();
        for (Entry& entry : m_entries) {
            release(entry.item);
        }
        m_entries.clear();
        return released;
    }

    [[nodiscard]] usize size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

private:
    struct Entry {
        T item;
        u64 frameSerial = 0;
    };
    std::deque<Entry> m_entries;
};

} // namespace vxe::rhi
