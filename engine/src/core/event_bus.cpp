#include "vxe/core/event_bus.hpp"

#include <vxe/core/profile.hpp>

#include <algorithm>

namespace vxe {

void Subscription::reset() noexcept {
    if (m_bus != nullptr) {
        m_bus->unsubscribe(m_type, m_id);
        m_bus = nullptr;
    }
}

EventBus::EventBus()
    : m_ownerThread(std::this_thread::get_id()) {}

EventBus::~EventBus() {
    // A subscription that outlives its bus would call into freed memory when
    // it is destroyed, so this is a bug in the shutdown order of the owner.
    VXE_ASSERT(m_liveSubscriptions == 0, "EventBus destroyed while subscriptions are still alive");
}

usize EventBus::nextTypeId() noexcept {
    static std::atomic<usize> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

usize EventBus::pendingCount() const {
    const std::scoped_lock lock(m_queueMutex);
    return m_queue.size();
}

void EventBus::dispatch() {
    VXE_PROFILE_FUNCTION();
    assertOwnerThread();

    // Take the whole queue at once, so events published while delivering
    // (from handlers or other threads) wait for the next dispatch.
    {
        const std::scoped_lock lock(m_queueMutex);
        m_delivering.swap(m_queue);
    }

    m_dispatching = true;
    for (const std::unique_ptr<QueuedEvent>& event : m_delivering) {
        const auto found = m_handlers.find(event->type());
        if (found == m_handlers.end()) {
            continue;
        }
        // New handlers are parked in m_pendingAdds while dispatching, so this
        // vector cannot grow (and move its elements) during the loop.
        for (const HandlerEntry& handler : found->second) {
            if (handler.active) {
                handler.callback(event->data());
            }
        }
    }
    m_dispatching = false;
    m_delivering.clear();

    for (auto& [type, entry] : m_pendingAdds) {
        m_handlers[type].push_back(std::move(entry));
    }
    m_pendingAdds.clear();

    if (m_needsCompaction) {
        for (auto& [type, handlers] : m_handlers) {
            std::erase_if(handlers, [](const HandlerEntry& h) { return !h.active; });
        }
        m_needsCompaction = false;
    }
}

void EventBus::unsubscribe(usize type, u64 id) noexcept {
    assertOwnerThread();
    --m_liveSubscriptions;

    // The handler may still be parked because it subscribed during dispatch.
    const auto parked = std::ranges::find_if(
        m_pendingAdds, [&](const auto& pending) { return pending.first == type && pending.second.id == id; });
    if (parked != m_pendingAdds.end()) {
        m_pendingAdds.erase(parked);
        return;
    }

    const auto found = m_handlers.find(type);
    if (found == m_handlers.end()) {
        return;
    }
    auto& handlers = found->second;
    const auto entry = std::ranges::find_if(handlers, [id](const HandlerEntry& h) { return h.id == id; });
    if (entry == handlers.end()) {
        return;
    }
    if (m_dispatching) {
        // Erasing now would shift the vector the dispatch loop is walking.
        // Mark it instead and remove it after the dispatch.
        entry->active = false;
        m_needsCompaction = true;
    } else {
        handlers.erase(entry);
    }
}

void EventBus::assertOwnerThread() const noexcept {
    VXE_ASSERT(std::this_thread::get_id() == m_ownerThread,
               "EventBus subscribe, unsubscribe and dispatch must run on the thread that created the bus");
}

} // namespace vxe
