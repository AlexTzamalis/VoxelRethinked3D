#include "vxe/core/log.hpp"

#include <vxe/core/profile.hpp>

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <system_error>

namespace vxe::log {
namespace {

// The registry of named categories. Registration happens at startup and from
// the console, so a plain mutex is fine. Logging itself never touches it.
struct Registry {
    std::mutex mutex;
    std::vector<Category*> categories;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

// The active backend. Logging threads only read the raw pointer, which is
// published with release and read with acquire. Ownership stays in
// ownedLogger, which only initialize() and shutdown() change. They must not
// run while other threads log (see the header).
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): the logger is global by nature.
std::atomic<spdlog::logger*> activeLogger{nullptr};
std::shared_ptr<spdlog::logger> ownedLogger;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Used when no logger exists. fwrite instead of fprintf avoids C varargs.
void writeToStderr(std::string_view text) noexcept {
    std::fwrite(text.data(), 1, text.size(), stderr);
}

spdlog::level::level_enum toSpdlog(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Critical:
        return spdlog::level::critical;
    case Level::Off:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

std::string_view nameOf(Level level) noexcept {
    switch (level) {
    case Level::Trace:
        return "trace";
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    case Level::Critical:
        return "critical";
    case Level::Off:
        return "off";
    }
    return "unknown";
}

void registerCategory(Category& category) {
    Registry& reg = registry();
    const std::scoped_lock lock(reg.mutex);
    if (std::ranges::find(reg.categories, &category) == reg.categories.end()) {
        reg.categories.push_back(&category);
    }
}

Category* findCategory(std::string_view name) {
    Registry& reg = registry();
    const std::scoped_lock lock(reg.mutex);
    const auto found =
        std::ranges::find_if(reg.categories, [name](const Category* c) { return c->name() == name; });
    return found == reg.categories.end() ? nullptr : *found;
}

std::vector<Category*> categories() {
    Registry& reg = registry();
    const std::scoped_lock lock(reg.mutex);
    return reg.categories;
}

bool initialize(const Config& config) {
    registerCategory(core);
    registerCategory(assertion);

    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        consoleSink->set_level(toSpdlog(config.consoleLevel));
        sinks.push_back(std::move(consoleSink));
    }

    bool fileOk = true;
    if (!config.filePath.empty()) {
        // spdlog reports file errors with exceptions. The engine does not throw
        // (Section 23.4), so the exception is converted to a false return here.
        try {
            std::error_code ignored;
            std::filesystem::create_directories(config.filePath.parent_path(), ignored);
            auto fileSink =
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.filePath.string(), true);
            fileSink->set_level(toSpdlog(config.fileLevel));
            sinks.push_back(std::move(fileSink));
        } catch (const spdlog::spdlog_ex& error) {
            writeToStderr(std::format("vxe: could not open log file '{}': {}\n", config.filePath.string(),
                                      error.what()));
            fileOk = false;
        }
    }

    auto logger = std::make_shared<spdlog::logger>("vxe", sinks.begin(), sinks.end());
    // Categories filter by level, so the backend lets everything through.
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::warn);
    // Example: [2026-10-27 14:03:11.482] [info ] [12345] [core] Message
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%-5l%$] [%t] %v");

    shutdown();
    ownedLogger = std::move(logger);
    activeLogger.store(ownedLogger.get(), std::memory_order_release);
    return fileOk;
}

void shutdown() {
    activeLogger.store(nullptr, std::memory_order_release);
    if (ownedLogger) {
        ownedLogger->flush();
        ownedLogger.reset();
    }
}

void flush() {
    if (spdlog::logger* logger = activeLogger.load(std::memory_order_acquire)) {
        logger->flush();
    }
}

namespace detail {

void write(const Category& category, Level level, std::string_view message) {
    spdlog::logger* logger = activeLogger.load(std::memory_order_acquire);
    if (logger == nullptr) {
        // Not initialized yet (or already shut down): stderr keeps the message.
        writeToStderr(std::format("[{}] [{}] {}\n", nameOf(level), category.name(), message));
        return;
    }
    logger->log(toSpdlog(level), "[{}] {}", category.name(), message);
    // Shows the message at the right moment in the Tracy timeline.
    VXE_PROFILE_MESSAGE(message.data(), message.size());
}

std::string& threadBuffer() {
    thread_local std::string buffer;
    return buffer;
}

} // namespace detail
} // namespace vxe::log
