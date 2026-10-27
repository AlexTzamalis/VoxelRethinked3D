#pragma once

#include <atomic>
#include <memory>

namespace vxe::jobs {

// Lets a job notice that its work is no longer wanted (Section 11.4: a chunk
// left render distance or changed while its mesh was being built).
//
// A CancelSource owns the flag and hands out tokens. Tokens are cheap to copy
// and safe to read from any thread. Cancellation is cooperative: a job that
// has not started yet is skipped by the job system, and a running job checks
// isCancelled() at convenient points and returns early.
class CancelToken {
public:
    // An empty token is never cancelled, so "no cancellation" needs no source.
    CancelToken() noexcept = default;

    [[nodiscard]] bool isCancelled() const noexcept {
        return m_flag && m_flag->load(std::memory_order_acquire);
    }
    [[nodiscard]] bool canBeCancelled() const noexcept { return m_flag != nullptr; }

private:
    friend class CancelSource;
    explicit CancelToken(std::shared_ptr<const std::atomic<bool>> flag) noexcept
        : m_flag(std::move(flag)) {}
    std::shared_ptr<const std::atomic<bool>> m_flag;
};

class CancelSource {
public:
    CancelSource()
        : m_flag(std::make_shared<std::atomic<bool>>(false)) {}

    // Marks every token of this source as cancelled. Cannot be undone; use a
    // new source for new work.
    void cancel() noexcept { m_flag->store(true, std::memory_order_release); }
    [[nodiscard]] bool isCancelled() const noexcept { return m_flag->load(std::memory_order_acquire); }
    [[nodiscard]] CancelToken token() const { return CancelToken(m_flag); }

private:
    std::shared_ptr<std::atomic<bool>> m_flag;
};

} // namespace vxe::jobs
