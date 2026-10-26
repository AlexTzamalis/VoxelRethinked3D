#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace vxe {

class EventBus;

// Keeps a handler registered for as long as it lives. Destroying it (or
// calling reset) unsubscribes, so a system that shuts down can never be
// called by accident. Move only. The bus must outlive its subscriptions.
class Subscription {
public:
    Subscription() noexcept = default;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept
        : m_bus(std::exchange(other.m_bus, nullptr))
        , m_type(other.m_type)
        , m_id(other.m_id) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            m_bus = std::exchange(other.m_bus, nullptr);
            m_type = other.m_type;
            m_id = other.m_id;
        }
        return *this;
    }
    ~Subscription() { reset(); }

    // Unsubscribes now. Safe to call more than once.
    void reset() noexcept;

    [[nodiscard]] bool isActive() const noexcept { return m_bus != nullptr; }

private:
    friend class EventBus;
    Subscription(EventBus* bus, usize type, u64 id) noexcept
        : m_bus(bus)
        , m_type(type)
        , m_id(id) {}

    EventBus* m_bus = nullptr;
    usize m_type = 0;
    u64 m_id = 0;
};

// A typed, deferred event bus.
//
// Any thread may publish() an event. Events are queued and delivered when the
// main thread calls dispatch(), in the order they were published. Deferring
// means a publisher never runs foreign code in the middle of its own work, and
// worker threads can notify the main thread without locks of their own.
//
//   struct WindowResized { u32 width, height; };
//   Subscription s = bus.subscribe<WindowResized>([](const WindowResized& e) { ... });
//   bus.publish(WindowResized{1920, 1080}); // From any thread.
//   bus.dispatch();                         // Main thread, once per frame.
//
// Rules for handlers (all enforced or made safe by the bus):
//   - Events published from a handler are delivered by the next dispatch(),
//     so a handler can never cause an endless loop within one dispatch.
//   - A handler may subscribe or unsubscribe. New handlers only receive events
//     from the next dispatch(); removed handlers receive nothing more.
//
// Each publish() allocates one small queue entry. The bus is meant for rare
// events (window, settings, world state changes), not per block or per frame
// data, which belong in plain arrays.
//
// Thread safety: publish() from any thread. Everything else from the thread
// that created the bus.
class EventBus {
public:
    EventBus();
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    template <typename Event>
    [[nodiscard]] Subscription subscribe(std::function<void(const Event&)> handler) {
        static_assert(std::is_same_v<Event, std::decay_t<Event>>, "Subscribe to the plain event type");
        assertOwnerThread();
        const u64 id = m_nextHandlerId++;
        HandlerEntry entry{
            .id = id,
            .callback = [handler = std::move(handler)](
                            const void* event) { handler(*static_cast<const Event*>(event)); },
            .active = true,
        };
        if (m_dispatching) {
            m_pendingAdds.emplace_back(typeIdOf<Event>(), std::move(entry));
        } else {
            m_handlers[typeIdOf<Event>()].push_back(std::move(entry));
        }
        ++m_liveSubscriptions;
        return Subscription(this, typeIdOf<Event>(), id);
    }

    // Queues an event for the next dispatch(). Safe from any thread.
    template <typename Event>
    void publish(Event&& event) {
        using Plain = std::decay_t<Event>;
        auto holder = std::make_unique<EventHolder<Plain>>(std::forward<Event>(event));
        const std::scoped_lock lock(m_queueMutex);
        m_queue.push_back(std::move(holder));
    }

    // Delivers every queued event to its handlers. Main thread only.
    void dispatch();

    // Number of events waiting for the next dispatch(). Any thread.
    [[nodiscard]] usize pendingCount() const;

private:
    friend class Subscription;

    struct HandlerEntry {
        u64 id = 0;
        std::function<void(const void*)> callback;
        bool active = true;
    };

    // Queued events are type erased behind this interface so one queue can
    // hold every event type in publish order.
    struct QueuedEvent {
        QueuedEvent() = default;
        QueuedEvent(const QueuedEvent&) = delete;
        QueuedEvent& operator=(const QueuedEvent&) = delete;
        QueuedEvent(QueuedEvent&&) = delete;
        QueuedEvent& operator=(QueuedEvent&&) = delete;
        virtual ~QueuedEvent() = default;
        [[nodiscard]] virtual usize type() const noexcept = 0;
        [[nodiscard]] virtual const void* data() const noexcept = 0;
    };

    template <typename Event>
    struct EventHolder final : QueuedEvent {
        template <typename Arg>
        explicit EventHolder(Arg&& value)
            : event(std::forward<Arg>(value)) {}
        [[nodiscard]] usize type() const noexcept override { return typeIdOf<Event>(); }
        [[nodiscard]] const void* data() const noexcept override { return &event; }
        Event event;
    };

    // A small unique number per event type, assigned on first use. Works
    // without RTTI and is the same in every translation unit, because the
    // static local of an inline function template exists once per type.
    static usize nextTypeId() noexcept;
    template <typename Event>
    static usize typeIdOf() noexcept {
        static const usize id = nextTypeId();
        return id;
    }

    void unsubscribe(usize type, u64 id) noexcept;
    void assertOwnerThread() const noexcept;

    std::thread::id m_ownerThread;
    HashMap<usize, std::vector<HandlerEntry>> m_handlers;
    std::vector<std::pair<usize, HandlerEntry>> m_pendingAdds;
    u64 m_nextHandlerId = 1;
    usize m_liveSubscriptions = 0;
    bool m_dispatching = false;
    bool m_needsCompaction = false;

    mutable std::mutex m_queueMutex;
    std::vector<std::unique_ptr<QueuedEvent>> m_queue;
    std::vector<std::unique_ptr<QueuedEvent>> m_delivering; // Reused to avoid reallocating.
};

} // namespace vxe
