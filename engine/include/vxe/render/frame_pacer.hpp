#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/platform/frame_limiter.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/types.hpp>

namespace vxe::render {

struct FramePacerDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
};

// Frame pacing (Section 8.5): the present mode and the frame rate caps.
//
// Settings, all live: Video.PresentMode (Immediate, Vsync, Adaptive,
// Mailbox), Video.FpsLimit (0 for none), Video.FpsLimitUnfocused (applies
// while the window has no focus, so a game in the background does not heat
// the GPU) and Video.FpsLimitInMenus (applies while a menu covers the world).
// The lowest limit that applies wins. The wait itself is a FrameLimiter:
// sleep, then a short spin, against a fixed schedule.
//
// Stat: Frame.LimiterWait (time spent waiting each frame).
//
// Thread safety: main thread only.
class FramePacer {
public:
    explicit FramePacer(const FramePacerDesc& desc);

    // Applies the present mode if it changed. Call once per frame before
    // presenting (cheap when nothing changed).
    void update();
    // The present mode in use: the chosen one, unless a frame rate limit replaces vertical sync.
    [[nodiscard]] rhi::PresentMode appliedMode() const noexcept { return m_appliedMode; }
    // Waits until the next frame may start. Call once per frame after present.
    void wait(bool windowFocused, bool inMenu);
    // The limit wait() uses in that state, 0 for none.
    [[nodiscard]] f64 limitFor(bool windowFocused, bool inMenu) const noexcept;

private:
    rhi::Device& m_device;
    stats::StatRegistry* m_stats;
    platform::FrameLimiter m_limiter;
    config::Setting<rhi::PresentMode> m_presentMode;
    config::Setting<i32> m_fpsLimit;
    config::Setting<i32> m_fpsLimitUnfocused;
    config::Setting<i32> m_fpsLimitInMenus;
    rhi::PresentMode m_appliedMode = rhi::PresentMode::Immediate;
    bool m_applied = false;
    stats::StatId m_waitStat;
};

} // namespace vxe::render
