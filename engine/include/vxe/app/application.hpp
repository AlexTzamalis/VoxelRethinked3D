#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/event_bus.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>
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
    // Frame cap while nothing renders yet (before the renderer exists, and
    // later when minimized). Keeps an empty loop from burning a core.
    u32 idleFpsLimit = 60;
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
// settings, window, jobs, then Application::onInit.
// Shutdown order: Application::onShutdown, then the job system (which
// finishes every queued job while everything a job could touch still
// exists), the application object, window, settings and event bus (after
// the application, which may hold subscriptions to both), file watcher, file
// system, platform, log. Setting changes are dispatched every frame, right
// before the event bus.
int runApplication(std::unique_ptr<Application> application, const EngineConfig& config);

} // namespace vxe
