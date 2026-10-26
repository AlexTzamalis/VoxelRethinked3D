// Entry point of Voxel Rethinked. For now it only starts logging and reports
// the version. Later this becomes the place that creates VoxelRethinkedApp and
// hands it to vxe::runApplication (Section 6.5 of the plan).
#include <vxe/core/log.hpp>
#include <vxe/core/version.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string_view>

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the level changes at runtime.
constinit vxe::log::Category gameLog{"game", vxe::log::Level::Info};

int run(std::span<char*> arguments) {
    // Runtime folders live next to the executable (Section 5 of the plan). The
    // platform module will provide the real executable folder later; argv[0]
    // is good enough until then.
    const std::filesystem::path exeFolder = arguments.empty()
                                                ? std::filesystem::path(".")
                                                : std::filesystem::path(arguments.front()).parent_path();

    vxe::log::Config logConfig;
    logConfig.filePath = exeFolder / "logs" / "latest.log";
    vxe::log::initialize(logConfig);
    vxe::log::registerCategory(gameLog);

    VXE_LOG_INFO(gameLog, "Voxel Rethinked {}", vxe::engineVersion());

    vxe::log::shutdown();
    return 0;
}

// Reports an exception that reached main. Logging allocates and could throw
// again, so a second failure falls back to a fixed stderr line.
void reportFatal(std::string_view what) noexcept {
    try {
        VXE_LOG_CRITICAL(gameLog, "Unhandled exception: {}", what);
        vxe::log::shutdown();
    } catch (...) {
        std::fputs("voxel_rethinked: fatal error while reporting an unhandled exception\n", stderr);
    }
}

} // namespace

int main(int argc, char** argv) {
    // Our code does not throw (Section 23.4), but the standard library can
    // (for example std::bad_alloc). Catching here turns that into a logged
    // error and a failing exit code instead of an unexplained crash.
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)));
    } catch (const std::exception& error) {
        reportFatal(error.what());
    } catch (...) {
        reportFatal("unknown exception");
    }
    return 1;
}
