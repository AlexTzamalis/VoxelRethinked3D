#include "vxe/render/frame_pacer.hpp"

#include <vxe/core/time.hpp>

#include <array>

namespace vxe::render {
namespace {

using Mode = config::EnumSettingDesc<rhi::PresentMode>::Option;
constexpr std::array<Mode, 4> kPresentModes{{
    {.value = rhi::PresentMode::Immediate, .name = "Immediate", .displayName = "Off (uncapped, may tear)"},
    {.value = rhi::PresentMode::Vsync, .name = "Vsync", .displayName = "Vertical sync"},
    {.value = rhi::PresentMode::Adaptive, .name = "Adaptive", .displayName = "Adaptive vertical sync"},
    {.value = rhi::PresentMode::Mailbox,
     .name = "Mailbox",
     .displayName = "Mailbox (low latency, no tearing)"},
}};

} // namespace

FramePacer::FramePacer(const FramePacerDesc& desc)
    : m_device(*desc.device)
    , m_stats(desc.stats) {
    config::SettingsRegistry& settings = *desc.settings;
    config::EnumSettingDesc<rhi::PresentMode> mode;
    mode.id = "Video.PresentMode";
    mode.displayName = "Vertical sync";
    mode.description = "How finished frames reach the screen.";
    mode.defaultValue = rhi::PresentMode::Vsync;
    mode.options = kPresentModes;
    m_presentMode = settings.registerEnum(mode);
    const auto limit = [&settings](std::string_view id, std::string_view name, std::string_view description,
                                   i32 defaultValue) {
        return settings.registerInt(
            {{.id = id, .displayName = name, .description = description}, defaultValue, 0, 1000, 1});
    };
    m_fpsLimit = limit("Video.FpsLimit", "Frame rate limit", "Frames per second cap. 0 for none.", 0);
    m_fpsLimitUnfocused = limit("Video.FpsLimitUnfocused", "Frame rate in the background",
                                "Cap while the window is not focused. 0 for none.", 30);
    m_fpsLimitInMenus = limit("Video.FpsLimitInMenus", "Frame rate in menus",
                              "Cap while a menu covers the world. 0 for none.", 120);
    if (m_stats != nullptr) {
        auto id = m_stats->registerStat({.name = "Frame.LimiterWait",
                                         .description = "Time the frame limiter waited this frame.",
                                         .type = stats::StatType::Timer});
        m_waitStat = id ? id.value() : stats::StatId{};
    }
}

void FramePacer::update() {
    const rhi::PresentMode mode = m_presentMode.get();
    if (!m_applied || mode != m_appliedMode) {
        m_device.setPresentMode(mode);
        m_appliedMode = mode;
        m_applied = true;
    }
}

f64 FramePacer::limitFor(bool windowFocused, bool inMenu) const noexcept {
    // The lowest of the caps that apply; 0 means "no cap" and never wins.
    i32 limit = m_fpsLimit.get();
    const auto apply = [&limit](i32 cap) {
        if (cap > 0 && (limit == 0 || cap < limit)) {
            limit = cap;
        }
    };
    if (!windowFocused) {
        apply(m_fpsLimitUnfocused.get());
    }
    if (inMenu) {
        apply(m_fpsLimitInMenus.get());
    }
    return static_cast<f64>(limit);
}

void FramePacer::wait(bool windowFocused, bool inMenu) {
    const TimePoint start = Clock::now();
    m_limiter.wait(limitFor(windowFocused, inMenu));
    if (m_stats != nullptr) {
        m_stats->addTime(m_waitStat, Clock::now() - start);
    }
}

} // namespace vxe::render
