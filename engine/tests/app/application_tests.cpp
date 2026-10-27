#include <vxe/app/application.hpp>

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::filesystem::path makeRuntimeFolder(std::string_view name) {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "app" / std::string(name);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder);
    return folder;
}

// Records every call the engine makes, so tests can check the lifecycle.
struct CallLog {
    std::vector<std::string> calls;
    vxe::u64 frames = 0;
};

class RecordingApp final : public vxe::Application {
public:
    RecordingApp(CallLog& log, bool failInit, bool pushQuit, bool quitAfterJobResult = false)
        : m_log(log)
        , m_failInit(failInit)
        , m_pushQuit(pushQuit)
        , m_quitAfterJobResult(quitAfterJobResult) {}

    vxe::Result<void> onInit(vxe::EngineContext& context) override {
        m_log.calls.emplace_back("init");
        m_log.calls.emplace_back(
            context.vfs.isMounted("config") && context.vfs.isMounted("saves") ? "mounted" : "not mounted");
        if (m_pushQuit) {
            // Simulates the user closing the window.
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        if (m_failInit) {
            return vxe::Error(vxe::ErrorCode::Unsupported, "test failure");
        }
        return {};
    }

    void onFrame(vxe::EngineContext& context, const vxe::FrameTime& time) override {
        if (m_log.frames == 0) {
            m_log.calls.emplace_back(time.frameIndex == 0 ? "first frame" : "bad first frame");
        }
        ++m_log.frames;
        // Jobs scheduled from a frame run and report back on the main thread.
        if (m_log.frames == 1) {
            static_cast<void>(context.jobs.schedule([&context, this] {
                context.jobs.runOnMainThread([this] {
                    m_log.calls.emplace_back("job result");
                    m_sawJobResult = true;
                });
            }));
        }
        // Quit by itself once the job result arrived, after at least 5 frames.
        if (m_quitAfterJobResult && m_sawJobResult && m_log.frames >= 5) {
            context.requestQuit();
        }
    }

    void onQuitRequested(vxe::EngineContext& context) override {
        m_log.calls.emplace_back("quit requested");
        context.requestQuit();
    }

    void onShutdown(vxe::EngineContext& /*context*/) override { m_log.calls.emplace_back("shutdown"); }

private:
    CallLog& m_log;
    bool m_failInit;
    bool m_pushQuit;
    bool m_quitAfterJobResult;
    bool m_sawJobResult = false;
};

vxe::EngineConfig headlessConfig(const std::filesystem::path& folder, vxe::u64 maxFrames) {
    return vxe::EngineConfig{.window = false,
                             .jobs = {.workerCount = 2, .ioThreadCount = 1},
                             .runtimeFolder = folder,
                             .maxFrames = maxFrames,
                             .idleFpsLimit = 0};
}

} // namespace

TEST_CASE("runApplication drives init, frames and shutdown in order") {
    const auto folder = makeRuntimeFolder("lifecycle");
    CallLog log;
    // maxFrames is only a safety net; the app quits once its job reported back.
    const int exitCode = vxe::runApplication(std::make_unique<RecordingApp>(log, false, false, true),
                                             headlessConfig(folder, 1'000'000));

    CHECK(exitCode == 0);
    CHECK(log.frames >= 5);
    CHECK(log.frames < 1'000'000);
    REQUIRE(log.calls.size() >= 4);
    CHECK(log.calls[0] == "init");
    CHECK(log.calls[1] == "mounted");
    CHECK(log.calls[2] == "first frame");
    CHECK(log.calls.back() == "shutdown");
    CHECK(std::ranges::find(log.calls, "job result") != log.calls.end());

    // The runtime folders and the log file exist next to the "executable".
    CHECK(std::filesystem::is_directory(folder / "config"));
    CHECK(std::filesystem::is_directory(folder / "saves"));
    CHECK(std::filesystem::exists(folder / "logs" / "latest.log"));
    // First start writes the settings file with its comments.
    CHECK(std::filesystem::exists(folder / "config" / "settings.toml"));
    CHECK(std::filesystem::exists(folder / "config" / "keybinds.toml"));
}

TEST_CASE("A quit request from the OS ends the loop through onQuitRequested") {
    const auto folder = makeRuntimeFolder("quit");
    CallLog log;
    const int exitCode =
        vxe::runApplication(std::make_unique<RecordingApp>(log, false, true), headlessConfig(folder, 0));
    CHECK(exitCode == 0);
    CHECK(log.frames == 1);
    CHECK(std::ranges::find(log.calls, "quit requested") != log.calls.end());
    CHECK(log.calls.back() == "shutdown");
}

TEST_CASE("A failing onInit exits with code 1 and still shuts down") {
    const auto folder = makeRuntimeFolder("fail");
    CallLog log;
    const int exitCode =
        vxe::runApplication(std::make_unique<RecordingApp>(log, true, false), headlessConfig(folder, 10));
    CHECK(exitCode == 1);
    CHECK(log.frames == 0);
    CHECK(log.calls.back() == "shutdown");
}
