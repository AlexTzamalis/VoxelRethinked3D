#pragma once

#include <vxe/jobs/job_system.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace vr {

// Runs disk work for a menu screen on an IO thread, so the menu never waits
// for the disk. The screen polls finished() in its update. The work owns what
// it needs (no pointers into the screen), since the player may leave the
// screen while it runs.
class BackgroundTask {
public:
    void start(vxe::jobs::JobSystem& jobs, std::function<void()> work) {
        auto state = std::make_shared<State>();
        state->work = std::move(work);
        m_state = state;
        const auto run = [state] {
            state->work();
            state->done.store(true, std::memory_order_release);
        };
        static_cast<void>(jobs.schedule(run, {vxe::jobs::Priority::Normal, {}, vxe::jobs::Pool::Io}));
    }
    [[nodiscard]] bool running() const noexcept { return m_state != nullptr; }
    // True once, when the work has ended: the screen then reads its results.
    [[nodiscard]] bool finished() noexcept {
        if (m_state == nullptr || !m_state->done.load(std::memory_order_acquire)) {
            return false;
        }
        m_state.reset();
        return true;
    }

private:
    struct State {
        std::function<void()> work;
        std::atomic<bool> done{false};
    };
    std::shared_ptr<State> m_state;
};

} // namespace vr
