// Entry point of Voxel Rethinked: builds the engine configuration from the
// command line and hands the game application to the engine's main loop
// (Section 6.5 of the plan).
//
// Command line:
//   --frames N        Quit after N frames (smoke tests and scripted runs).
//   --headless        Run without a window.
//   --exec COMMAND    Run a console command at startup, for example
//                     --exec "set Video.FpsLimit 144". May repeat.
#include "voxel_rethinked_app.hpp"

#include <vxe/app/application.hpp>

#include <charconv>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace {

vxe::EngineConfig configFromArguments(std::span<char*> arguments) {
    vxe::EngineConfig config;
    config.windowDesc.title = "Voxel Rethinked";
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "--headless") {
            config.window = false;
        } else if (argument == "--exec" && i + 1 < arguments.size()) {
            config.startupCommands.emplace_back(arguments[++i]);
        } else if (argument == "--frames" && i + 1 < arguments.size()) {
            const std::string_view value = arguments[++i];
            vxe::u64 frames = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), frames).ec == std::errc{}) {
                config.maxFrames = frames;
            }
        }
    }
    return config;
}

// Reports an exception that reached main. Logging is already shut down at
// this point (or broken), so it goes straight to stderr.
void reportFatal(const char* what) noexcept {
    std::fputs("voxel_rethinked: unhandled exception: ", stderr);
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
}

} // namespace

int main(int argc, char** argv) {
    // Our code does not throw (Section 23.4), but the standard library can
    // (for example std::bad_alloc). Catching here turns that into a clear
    // message and a failing exit code instead of an unexplained crash.
    try {
        const vxe::EngineConfig config = configFromArguments(std::span(argv, static_cast<std::size_t>(argc)));
        return vxe::runApplication(std::make_unique<vr::VoxelRethinkedApp>(), config);
    } catch (const std::exception& error) {
        reportFatal(error.what());
    } catch (...) {
        reportFatal("unknown exception");
    }
    return 1;
}
