#include "vxe/app/application.hpp"

#include <vxe/config/settings_file.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/core/version.hpp>

#include <chrono>
#include <thread>
#include <variant>
#include <vector>

namespace vxe {
namespace {

// Mounts the four runtime folders (Section 5). config/ and saves/ are
// created on first start; assets/ and textures/ are read only and only
// mounted when present, so a fresh build without assets still starts.
Result<void> mountRuntimeFolders(io::Vfs& vfs, const std::filesystem::path& root) {
    VXE_TRY(vfs.mount("config", root / "config",
                      io::MountOptions{.access = io::MountAccess::ReadWrite, .createIfMissing = true}));
    VXE_TRY(vfs.mount("saves", root / "saves",
                      io::MountOptions{.access = io::MountAccess::ReadWrite, .createIfMissing = true}));
    for (const char* name : {"assets", "textures"}) {
        std::error_code error;
        if (std::filesystem::is_directory(root / name, error)) {
            VXE_TRY(vfs.mount(name, root / name, io::MountOptions{.access = io::MountAccess::ReadOnly}));
        } else {
            VXE_LOG_WARN(log::core, "No '{}' folder next to the executable, '{}:/' is not mounted", name,
                         name);
        }
    }
    return {};
}

// Sleeps the rest of a frame when nothing renders, so an empty loop does not
// spin a core at 100 percent. The renderer replaces this with real frame
// pacing (Section 8.5).
void limitIdleFrame(const TimePoint frameStart, u32 fpsLimit) {
    if (fpsLimit == 0) {
        return;
    }
    const Duration budget = std::chrono::duration_cast<Duration>(std::chrono::seconds(1)) / fpsLimit;
    const Duration used = Clock::now() - frameStart;
    if (used < budget) {
        std::this_thread::sleep_for(budget - used);
    }
}

} // namespace

int runApplication(std::unique_ptr<Application> application, const EngineConfig& config) {
    VXE_ASSERT(application != nullptr, "runApplication needs an application");

    const std::filesystem::path root =
        config.runtimeFolder.empty() ? platform::Platform::executableFolder() : config.runtimeFolder;
    log::initialize(log::Config{.filePath = root / "logs" / "latest.log"});
    profiling::setThreadName("Main");
    VXE_LOG_INFO(log::core, "vxe engine {} starting in '{}'", engineVersion(), root.string());

    // Each system lives in its own unique_ptr so it can be destroyed exactly
    // in the documented order below, whatever happened before.
    auto platformResult = platform::Platform::create(platform::PlatformConfig{.video = config.window});
    if (!platformResult) {
        VXE_LOG_CRITICAL(log::core, "Startup failed: {}", platformResult.error().describe());
        log::shutdown();
        return 1;
    }
    std::unique_ptr<platform::Platform> platform = std::move(platformResult).value();

    auto vfs = std::make_unique<io::Vfs>();
    if (Result<void> mounted = mountRuntimeFolders(*vfs, root); !mounted) {
        VXE_LOG_CRITICAL(log::core, "Startup failed: {}", mounted.error().describe());
        platform.reset();
        log::shutdown();
        return 1;
    }

    auto fileWatcher = std::make_unique<io::FileWatcher>();
    auto events = std::make_unique<EventBus>();
    auto settings = std::make_unique<config::SettingsRegistry>();
    auto input = std::make_unique<input::InputSystem>();

    std::unique_ptr<platform::Window> window;
    if (config.window) {
        auto windowResult = platform::Window::create(*platform, config.windowDesc);
        if (!windowResult) {
            VXE_LOG_CRITICAL(log::core, "Startup failed: {}", windowResult.error().describe());
            settings.reset();
            events.reset();
            fileWatcher.reset();
            platform.reset();
            log::shutdown();
            return 1;
        }
        window = std::move(windowResult).value();
    }

    auto jobs = std::make_unique<jobs::JobSystem>(config.jobs);
    VXE_LOG_INFO(log::core, "Job system: {} workers, {} IO threads", jobs->workerCount(),
                 jobs->ioThreadCount());

    std::vector<platform::PlatformEvent> frameEvents;
    frameEvents.reserve(64);
    EngineContext context{.platform = *platform,
                          .window = window.get(),
                          .jobs = *jobs,
                          .vfs = *vfs,
                          .fileWatcher = *fileWatcher,
                          .events = *events,
                          .settings = *settings,
                          .input = *input,
                          .frameEvents = {},
                          .quitRequested = false};

    int exitCode = 0;
    if (Result<void> initialized = application->onInit(context); !initialized) {
        VXE_LOG_CRITICAL(log::core, "Application startup failed: {}", initialized.error().describe());
        exitCode = 1;
        context.requestQuit();
    }

    // Settings are loaded after onInit, because that is where they are
    // registered (Section 6.5). On first start the file does not exist and is
    // written with the defaults, so players find a commented file to edit.
    const Result<std::filesystem::path> settingsPath = vfs->resolveForWrite("config:/settings.toml");
    if (exitCode == 0 && settingsPath) {
        Result<config::LoadReport> loaded = config::loadSettingsFromFile(*settings, settingsPath.value());
        if (loaded) {
            for (const std::string& warning : loaded.value().warnings) {
                VXE_LOG_WARN(log::core, "settings.toml: {}", warning);
            }
            VXE_LOG_INFO(log::core, "Loaded {} settings from config/settings.toml", loaded.value().applied);
        } else if (loaded.error().code() == ErrorCode::NotFound) {
            if (Result<void> saved = config::saveSettingsToFile(*settings, settingsPath.value()); !saved) {
                VXE_LOG_WARN(log::core, "Could not write default settings: {}", saved.error().describe());
            }
        } else {
            VXE_LOG_ERROR(log::core, "settings.toml was not loaded, using defaults: {}",
                          loaded.error().describe());
        }
    }

    FrameTimer frameTimer;
    u64 frames = 0;
    while (!context.isQuitRequested()) {
        VXE_PROFILE_SCOPE("Frame");
        const TimePoint frameStart = Clock::now();
        const FrameTime time = frameTimer.tick(frameStart);

        frameEvents.clear();
        static_cast<void>(platform->pollEvents(frameEvents));
        context.frameEvents = frameEvents;
        input->update(frameEvents);
        for (const platform::PlatformEvent& event : frameEvents) {
            if (std::holds_alternative<platform::QuitRequested>(event)) {
                application->onQuitRequested(context);
            }
        }

        static_cast<void>(fileWatcher->poll());
        static_cast<void>(jobs->runMainThreadTasks());
        static_cast<void>(settings->dispatchChanges());
        events->dispatch();
        application->onFrame(context, time);

        ++frames;
        if (config.maxFrames != 0 && frames >= config.maxFrames) {
            context.requestQuit();
        }
        // Nothing renders yet, so every frame is an idle frame.
        limitIdleFrame(frameStart, config.idleFpsLimit);
        VXE_PROFILE_FRAME();
    }
    VXE_LOG_INFO(log::core, "Main loop ended after {} frames", frames);

    // Shutdown in the documented order (see application.hpp). Settings are
    // saved after onShutdown, so changes the application makes there persist.
    application->onShutdown(context);
    if (exitCode == 0 && settingsPath) {
        if (Result<void> saved = config::saveSettingsToFile(*settings, settingsPath.value()); !saved) {
            VXE_LOG_WARN(log::core, "Could not save settings: {}", saved.error().describe());
        }
    }
    jobs.reset();
    application.reset();
    window.reset();
    input.reset();
    settings.reset(); // After the application, which may hold setting subscriptions.
    events.reset();
    fileWatcher.reset();
    vfs.reset();
    platform.reset();
    VXE_LOG_INFO(log::core, "Shut down cleanly");
    log::shutdown();
    return exitCode;
}

} // namespace vxe
