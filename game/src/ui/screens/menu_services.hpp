#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/input/input.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/texture_pack_manager.hpp>
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
    std::filesystem::path saves;                      // The worlds' folder (saves:/).
    vxe::render::TexturePackManager* packs = nullptr; // Nothing without a renderer.
    vxe::platform::Platform* platform = nullptr;      // File pickers.
    vxe::platform::Window* window = nullptr;          // Parent of file pickers; may be null.
};

} // namespace vr
