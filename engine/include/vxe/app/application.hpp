#pragma once

#include <vxe/config/console.hpp>
#include <vxe/config/settings.hpp>
#include <vxe/core/event_bus.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>
#include <vxe/io/file_watcher.hpp>
#include <vxe/io/vfs.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/platform/events.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vxe::rhi {
class Device;
}
namespace vxe::render {
class Renderer;
}
namespace vxe::debug {
class DebugOverlay;
class DeveloperConsole;
} // namespace vxe::debug

namespace vxe {

// How runApplication sets the engine up.
struct EngineConfig {
    // Open a window. Off for tools, tests and a future dedicated server.
    bool window = true;
    platform::WindowDesc windowDesc{};
    jobs::JobSystemConfig jobs{};
    // Folder that holds config/, saves/, textures/, assets/ and logs/.
    // Empty means the folder of the executable (Section 5).
    std::filesystem::path runtimeFolder;
    // Stop after this many frames, 0 for no limit. For smoke tests and
    // scripted runs (for example --frames 120 on the command line).
    u64 maxFrames = 0;
    // Frame cap of a loop without a renderer (headless runs). Keeps an empty
    // loop from burning a core. With a renderer, Video.FpsLimit and the other
    // frame pacing settings apply instead (Section 8.5).
    u32 idleFpsLimit = 60;
    // With a window: create the graphics device and the standard renderer
    // (render::Renderer). Off for tests that only need a window.
    bool renderer = true;
    // With a renderer: the debug UI, performance overlay (F3) and developer
    // console (Grave).
    bool debugTools = true;
    // Engine shaders. Empty means shaders/ next to the executable, where the
    // build copies them.
    std::filesystem::path shaderFolder{};
    // Console commands run once everything is registered and the settings
    // file is loaded, for example "set Video.FpsLimit 144" (--exec).
    std::vector<std::string> startupCommands{};
};

// The engine services an application uses. A plain struct of references, so
// there are no global singletons (Section 6.5): everything an application
// needs is passed to it. Services that do not exist in a configuration (the
// window in a headless run) are null pointers.
struct EngineContext {
    platform::Platform& platform;
    platform::Window* window; // Null when EngineConfig::window is false.
    jobs::JobSystem& jobs;
    io::Vfs& vfs;
    io::FileWatcher& fileWatcher;
    EventBus& events;
    config::SettingsRegistry& settings;
    input::InputSystem& input; // Updated from frameEvents before onFrame.
    stats::StatRegistry& stats;
    config::CommandRegistry& commands; // Console commands; the game adds its own.

    // Rendering, null without a window or with EngineConfig::renderer off.
    rhi::Device* device;
    render::Renderer* renderer;
    // Debug tools, null when EngineConfig::debugTools is off.
    debug::DebugOverlay* overlay;
    debug::DeveloperConsole* console;

    // Set by the application while a menu is up, so Video.FpsLimitInMenus
    // applies.
    bool inMenu = false;

    // The OS events of the current frame, in order. Valid during onFrame.
    std::span<const platform::PlatformEvent> frameEvents;

    // Ends the main loop after the current frame.
    void requestQuit() noexcept { quitRequested = true; }
    [[nodiscard]] bool isQuitRequested() const noexcept { return quitRequested; }

    bool quitRequested = false;
};

// The base class of every program built on the engine (the game, tools, and
// tests with a window). The engine owns the main loop so that timing,
// profiling and shutdown order are identical for all of them (Section 6.5).
class Application {
public:
    Application() = default;
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;
    virtual ~Application() = default;

    // Called once after every engine system exists. Register settings,
    // keybinds, block data and screens here. Returning an error stops the
    // program before the first frame.
    virtual Result<void> onInit(EngineContext& context) = 0;

    // Called every frame with the real elapsed time. Use it for things that
    // should feel smooth, like the camera and UI animation.
    virtual void onFrame(EngineContext& context, const FrameTime& time) = 0;

    // Called when the user asks to quit (window close, Alt+F4). The default
    // quits right away; a game can override it to ask "Save before quitting?".
    virtual void onQuitRequested(EngineContext& context) { context.requestQuit(); }

    // Called once before engine systems are destroyed. Save and flush here.
    virtual void onShutdown(EngineContext& context) = 0;
};

// Runs the main loop until the application asks to quit, then shuts every
// system down in a fixed order (the reverse of creation). Returns a process
// exit code: 0 on a normal exit, 1 when startup failed.
//
// Startup order: log, platform, virtual file system, file watcher, event bus,
// settings, input, stats, console commands, window, then (with a window and
// EngineConfig::renderer) the graphics device, debug tools, renderer and
// frame pacer, then jobs and Application::onInit, then the settings and
// keybind files and the startup commands.
//
// A frame: OS events and input, the debug UI's frame start (overlay, stat
// merge), file watching, main thread jobs, setting changes, events,
// Application::onFrame, the debug UI's frame end, then the renderer's
// endFrame (record, submit, present) and the frame pacer's wait.
//
// Shutdown order: Application::onShutdown, the settings and keybind files,
// the job system (which finishes every queued job while everything a job
// could touch still exists), the application object, then frame pacer,
// renderer, debug tools and the device (after the GPU is idle, with a leak
// report), window, commands, input, settings (after everything that may hold
// setting handles), stats, event bus, file watcher, file system, platform,
// log.
int runApplication(std::unique_ptr<Application> application, const EngineConfig& config);

} // namespace vxe
