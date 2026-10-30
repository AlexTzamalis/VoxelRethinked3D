#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/input/input.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/ui/ui_context.hpp>

#include <filesystem>

namespace vr {

// What menu screens need from the engine, so opening one screen from another
// does not pass each service along. The references outlive every screen.
struct MenuServices {
    vxe::config::SettingsRegistry& settings;
    vxe::input::InputSystem& input;
    vxe::ui::UiContext& ui;
    vxe::jobs::JobSystem& jobs;
    std::filesystem::path saves; // The worlds' folder (saves:/).
};

} // namespace vr
