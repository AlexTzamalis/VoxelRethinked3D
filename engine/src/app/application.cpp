#include "vxe/app/application.hpp"

#include <vxe/config/settings_file.hpp>
#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>
#include <vxe/core/version.hpp>
#include <vxe/debug/debug_ui.hpp>
#include <vxe/debug/developer_console.hpp>
#include <vxe/debug/overlay.hpp>
#include <vxe/input/keybinds.hpp>
#include <vxe/render/frame_pacer.hpp>
#include <vxe/render/renderer.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/rhi_gl/gl_device.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <string>
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

// Sleeps the rest of a frame when nothing renders (headless runs), so an
// empty loop does not spin a core at 100 percent. With a renderer the frame
// pacer does this job (Section 8.5).
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

// Loads one of the player's files (settings.toml, keybinds.toml). On first
// start the file does not exist and is written with the defaults, so players
// find a commented file to edit. Problems are logged and never stop the game.
template <typename LoadFn, typename SaveFn>
void loadOrCreateUserFile(const Result<std::filesystem::path>& path, const LoadFn& load, const SaveFn& save) {
    if (!path) {
        return;
    }
    const std::string name = path.value().filename().string();
    auto loaded = load(path.value());
    if (loaded) {
        for (const std::string& warning : loaded.value().warnings) {
            VXE_LOG_WARN(log::core, "{}: {}", name, warning);
        }
        VXE_LOG_INFO(log::core, "Loaded {} ({} entries)", name, loaded.value().applied);
    } else if (loaded.error().code() == ErrorCode::NotFound) {
        if (Result<void> saved = save(path.value()); !saved) {
            VXE_LOG_WARN(log::core, "Could not write the default {}: {}", name, saved.error().describe());
        }
    } else {
        VXE_LOG_ERROR(log::core, "{} was not loaded, using defaults: {}", name, loaded.error().describe());
    }
}

template <typename SaveFn>
void saveUserFile(const Result<std::filesystem::path>& path, const SaveFn& save) {
    if (!path) {
        return;
    }
    if (Result<void> saved = save(path.value()); !saved) {
        VXE_LOG_WARN(log::core, "Could not save {}: {}", path.value().filename().string(),
                     saved.error().describe());
    }
}

// The services runApplication creates, owned in one place so that every
// exit path (startup failure or a normal quit) destroys them in the same
// documented order.
struct EngineServices {
    std::unique_ptr<platform::Platform> platform;
    std::unique_ptr<io::Vfs> vfs;
    std::unique_ptr<io::FileWatcher> fileWatcher;
    std::unique_ptr<EventBus> events;
    std::unique_ptr<config::SettingsRegistry> settings;
    std::unique_ptr<input::InputSystem> input;
    std::unique_ptr<stats::StatRegistry> stats;
    std::unique_ptr<config::CommandRegistry> commands;
    std::unique_ptr<platform::Window> window;
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<debug::DebugUi> debugUi;
    std::unique_ptr<debug::DebugOverlay> overlay;
    std::unique_ptr<debug::DeveloperConsole> console;
    std::unique_ptr<render::Renderer> renderer;
    std::unique_ptr<render::FramePacer> pacer;
    std::unique_ptr<jobs::JobSystem> jobs;

    // Everything that renders or holds GPU objects, before the device; then
    // the device, after the GPU finished and with a leak report (E6.6).
    void destroyRendering() {
        pacer.reset();
        renderer.reset();
        console.reset();
        overlay.reset();
        debugUi.reset();
        if (device) {
            device->waitIdle();
            for (const std::string& leak : device->liveResources()) {
                VXE_LOG_ERROR(log::core, "Leaked {}", leak);
            }
            device.reset();
        }
    }

    // The documented order (application.hpp); the application object itself
    // is destroyed by the caller between the job system and rendering.
    void destroyRest() {
        destroyRendering();
        window.reset();
        commands.reset();
        input.reset();
        settings.reset(); // After everything that may hold setting handles or subscriptions.
        stats.reset();
        events.reset();
        fileWatcher.reset();
        vfs.reset();
        platform.reset();
    }
};

// The graphics device, renderer, frame pacer and debug tools (Section 6.5,
// G0.1): what a windowed program needs to show anything.
Result<void> createRendering(EngineServices& services, const EngineConfig& config,
                             const std::filesystem::path& root) {
    auto device = rhi::createGlDevice(*services.window);
    if (!device) {
        return device.error();
    }
    services.device = std::move(device.value());
    const std::filesystem::path shaderFolder =
        config.shaderFolder.empty() ? root / "shaders" : config.shaderFolder;
    const rhi::ShaderFileReader shaders = rhi::folderShaderReader(shaderFolder);

    input::InputContextId debugContext{};
    if (config.debugTools) {
        auto ui = debug::DebugUi::create(*services.device, *services.input, {.shaders = shaders});
        if (!ui) {
            return ui.error();
        }
        services.debugUi = std::move(ui.value());
        services.overlay =
            std::make_unique<debug::DebugOverlay>(debug::OverlayDesc{.settings = services.settings.get(),
                                                                     .input = services.input.get(),
                                                                     .stats = services.stats.get(),
                                                                     .device = services.device.get(),
                                                                     .captureFolder = root / "captures"});
        services.console = std::make_unique<debug::DeveloperConsole>(
            debug::ConsoleDesc{.commands = services.commands.get(),
                               .settings = services.settings.get(),
                               .input = services.input.get(),
                               .stats = services.stats.get(),
                               .toggleContext = services.overlay->inputContext()});
        debugContext = services.overlay->inputContext();
    }
    auto renderer = render::Renderer::create({.device = services.device.get(),
                                              .settings = services.settings.get(),
                                              .stats = services.stats.get(),
                                              .input = services.input.get(),
                                              .debugContext = debugContext,
                                              .shaders = shaders});
    if (!renderer) {
        return renderer.error();
    }
    services.renderer = std::move(renderer.value());
    if (services.overlay) {
        services.overlay->addOverviewLine(
            [chunks = &services.renderer->chunks()] { return chunks->cullingSummary(); });
    }
    if (services.debugUi) {
        services.renderer->setWindowOverlay(
            [ui = services.debugUi.get()](rhi::CommandList& commands) { ui->record(commands); });
    }
    services.pacer = std::make_unique<render::FramePacer>(render::FramePacerDesc{
        .device = services.device.get(), .settings = services.settings.get(), .stats = services.stats.get()});
    return {};
}

// The debug UI's part of a frame before the application runs, in the order
// DebugOverlay documents: events, the finished frame's times, the stat
// merge, then a new UI frame the application may add windows to.
void beginDebugFrame(EngineServices& services, std::span<const platform::PlatformEvent> events,
                     const FrameTime& time) {
    if (!services.debugUi) {
        services.stats->endFrame(time.deltaSeconds);
        return;
    }
    services.debugUi->processEvents(events);
    services.overlay->processEvents(events);
    services.overlay->recordFrame(toMilliseconds(time.rawDelta),
                                  services.renderer ? services.renderer->gpuMilliseconds() : std::nullopt);
    services.stats->endFrame(time.deltaSeconds);
    services.debugUi->beginFrame(time.deltaSeconds, Vec2(services.window->size()),
                                 Vec2(services.window->pixelSize()));
    services.overlay->update(time.deltaSeconds);
    services.console->update();
}

void endDebugFrame(EngineServices& services) {
    if (services.debugUi) {
        services.debugUi->endFrame();
        services.window->setTextInputEnabled(services.debugUi->wantsTextInput());
    }
}

// The player's settings.toml and keybinds.toml. They are loaded after
// onInit, because that is where settings and actions are registered
// (Section 6.5), so the player's values replace the defaults; they are saved
// after onShutdown, so changes made there persist.
class UserFiles {
public:
    UserFiles(EngineServices& services, const config::Setting<bool>& bindByKeyName)
        : m_services(&services)
        , m_bindByKeyName(&bindByKeyName)
        , m_settingsPath(services.vfs->resolveForWrite("config:/settings.toml"))
        , m_keybindsPath(services.vfs->resolveForWrite("config:/keybinds.toml")) {}

    void load() {
        config::SettingsRegistry& settings = *m_services->settings;
        input::InputSystem& input = *m_services->input;
        loadOrCreateUserFile(
            m_settingsPath,
            [&settings](const auto& path) { return config::loadSettingsFromFile(settings, path); },
            [this](const auto& path) { return saveSettings(path); });
        loadOrCreateUserFile(
            m_keybindsPath, [&input](const auto& path) { return input::loadKeybindsFromFile(input, path); },
            [this](const auto& path) { return saveKeybinds(path); });
    }

    void save() {
        saveUserFile(m_settingsPath, [this](const auto& path) { return saveSettings(path); });
        saveUserFile(m_keybindsPath, [this](const auto& path) { return saveKeybinds(path); });
    }

private:
    [[nodiscard]] Result<void> saveSettings(const std::filesystem::path& path) const {
        return config::saveSettingsToFile(*m_services->settings, path);
    }
    [[nodiscard]] Result<void> saveKeybinds(const std::filesystem::path& path) const {
        const auto mode = m_bindByKeyName->get() ? input::KeyNameMode::Layout : input::KeyNameMode::Physical;
        return input::saveKeybindsToFile(*m_services->input, path, mode);
    }

    EngineServices* m_services;
    const config::Setting<bool>* m_bindByKeyName;
    Result<std::filesystem::path> m_settingsPath;
    Result<std::filesystem::path> m_keybindsPath;
};

// EngineConfig::startupCommands, through the console when there is one (so
// they show in its history) and straight to the command registry otherwise.
void runStartupCommands(EngineServices& services, std::span<const std::string> commands) {
    for (const std::string& command : commands) {
        if (services.console) {
            static_cast<void>(services.console->execute(command));
            continue;
        }
        const config::CommandOutput output = services.commands->execute(command);
        for (const std::string& line : output.lines) {
            VXE_LOG_INFO(log::core, "> {}: {}", command, line);
        }
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

    EngineServices services;
    const auto failStartup = [&services](const Error& error) {
        VXE_LOG_CRITICAL(log::core, "Startup failed: {}", error.describe());
        services.jobs.reset();
        services.destroyRest();
        log::shutdown();
        return 1;
    };

    auto platformResult = platform::Platform::create(platform::PlatformConfig{.video = config.window});
    if (!platformResult) {
        return failStartup(platformResult.error());
    }
    services.platform = std::move(platformResult).value();
    services.vfs = std::make_unique<io::Vfs>();
    if (Result<void> mounted = mountRuntimeFolders(*services.vfs, root); !mounted) {
        return failStartup(mounted.error());
    }
    services.fileWatcher = std::make_unique<io::FileWatcher>();
    services.events = std::make_unique<EventBus>();
    services.settings = std::make_unique<config::SettingsRegistry>();
    services.input = std::make_unique<input::InputSystem>();
    services.stats = std::make_unique<stats::StatRegistry>();
    services.commands = std::make_unique<config::CommandRegistry>();
    config::registerSettingsCommands(*services.commands, *services.settings);

    if (config.window) {
        auto windowResult = platform::Window::create(*services.platform, config.windowDesc);
        if (!windowResult) {
            return failStartup(windowResult.error());
        }
        services.window = std::move(windowResult).value();
        if (config.renderer) {
            if (Result<void> rendering = createRendering(services, config, root); !rendering) {
                return failStartup(rendering.error());
            }
        }
    }

    services.jobs = std::make_unique<jobs::JobSystem>(config.jobs);
    VXE_LOG_INFO(log::core, "Job system: {} workers, {} IO threads", services.jobs->workerCount(),
                 services.jobs->ioThreadCount());

    // Engine owned settings, registered before the application's own.
    const config::Setting<bool> bindByKeyName = input::registerInputSettings(*services.settings);

    std::vector<platform::PlatformEvent> frameEvents;
    frameEvents.reserve(64);
    EngineContext context{.platform = *services.platform,
                          .window = services.window.get(),
                          .jobs = *services.jobs,
                          .vfs = *services.vfs,
                          .fileWatcher = *services.fileWatcher,
                          .events = *services.events,
                          .settings = *services.settings,
                          .input = *services.input,
                          .stats = *services.stats,
                          .commands = *services.commands,
                          .device = services.device.get(),
                          .renderer = services.renderer.get(),
                          .overlay = services.overlay.get(),
                          .console = services.console.get(),
                          .inMenu = false,
                          .frameEvents = {},
                          .quitRequested = false};

    int exitCode = 0;
    if (Result<void> initialized = application->onInit(context); !initialized) {
        VXE_LOG_CRITICAL(log::core, "Application startup failed: {}", initialized.error().describe());
        exitCode = 1;
        context.requestQuit();
    }

    UserFiles files(services, bindByKeyName);
    if (exitCode == 0) {
        files.load();
        runStartupCommands(services, config.startupCommands);
    }

    FrameTimer frameTimer;
    u64 frames = 0;
    bool focused = true;
    while (!context.isQuitRequested()) {
        VXE_PROFILE_SCOPE("Frame");
        const TimePoint frameStart = Clock::now();
        const FrameTime time = frameTimer.tick(frameStart);

        frameEvents.clear();
        static_cast<void>(services.platform->pollEvents(frameEvents));
        context.frameEvents = frameEvents;
        services.input->update(frameEvents);
        for (const platform::PlatformEvent& event : frameEvents) {
            if (std::holds_alternative<platform::QuitRequested>(event)) {
                application->onQuitRequested(context);
            } else if (const auto* focus = std::get_if<platform::WindowFocusChanged>(&event)) {
                focused = focus->focused;
            }
        }
        beginDebugFrame(services, frameEvents, time);

        static_cast<void>(services.fileWatcher->poll());
        static_cast<void>(services.jobs->runMainThreadTasks());
        static_cast<void>(services.settings->dispatchChanges());
        services.events->dispatch();
        application->onFrame(context, time);
        endDebugFrame(services);

        if (services.renderer) {
            services.pacer->update();
            if (Result<void> rendered = services.renderer->endFrame(time); !rendered) {
                VXE_LOG_ERROR(log::core, "Frame failed: {}", rendered.error().describe());
                context.requestQuit();
            }
            // Video.FpsLimit and friends, read every frame so changes apply at once.
            services.pacer->wait(focused, context.inMenu);
        } else {
            limitIdleFrame(frameStart, config.idleFpsLimit);
        }

        ++frames;
        if (config.maxFrames != 0 && frames >= config.maxFrames) {
            context.requestQuit();
        }
        VXE_PROFILE_FRAME();
    }
    VXE_LOG_INFO(log::core, "Main loop ended after {} frames", frames);
    if (services.overlay) {
        const debug::FrameTimeSummary recent = services.overlay->recentFrames();
        VXE_LOG_INFO(log::core,
                     "Recent frames: {:.0f} FPS average, 1% low {:.0f}, {:.3f} ms average, GPU {:.3f} ms",
                     recent.averageFps, recent.low1Fps, recent.averageMs,
                     services.stats->average(services.stats->find("Frame.GpuTime"), 60));
        if (services.renderer) {
            VXE_LOG_INFO(log::core, "{}", services.renderer->chunks().cullingSummary());
        }
    }

    // Shutdown in the documented order (see application.hpp). Settings are
    // saved after onShutdown, so changes the application makes there persist.
    application->onShutdown(context);
    if (exitCode == 0) {
        files.save();
    }
    services.jobs.reset();
    application.reset();
    services.destroyRest();
    VXE_LOG_INFO(log::core, "Shut down cleanly");
    log::shutdown();
    return exitCode;
}

} // namespace vxe
