#pragma once

#include <vxe/core/types.hpp>

#include <atomic>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Engine logging.
//
// Every message belongs to a Category (for example "core" or "render") with
// its own minimum level, so one noisy system can be turned up or down alone.
// Messages are formatted with std::format and written to the console and to
// logs/latest.log. The backend (spdlog) is an implementation detail of
// log.cpp and never appears in a header.
//
// Usage:
//   VXE_LOG_INFO(vxe::log::core, "Loaded {} chunks in {:.2f} ms", count, ms);
//
// The level check happens before any formatting, so a disabled message costs
// one atomic load. Format strings are checked at compile time.
//
// Thread safety: logging is safe from any thread. initialize() and shutdown()
// must be called from the main thread while no other thread is logging.
namespace vxe::log {

enum class Level : u8 {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off, // Only valid as a minimum level: disables a category.
};

// Returns the lowercase name of a level, for example "warn".
[[nodiscard]] std::string_view toString(Level level) noexcept;

// A named group of log messages with its own minimum level. Categories are
// usually global objects that live for the whole program, so registering one
// stores a pointer to it. They are neither copyable nor movable for that reason.
class Category {
public:
    constexpr Category(std::string_view name, Level minimumLevel) noexcept
        : m_name(name)
        , m_level(minimumLevel) {}
    Category(const Category&) = delete;
    Category& operator=(const Category&) = delete;
    Category(Category&&) = delete;
    Category& operator=(Category&&) = delete;
    ~Category() = default;

    [[nodiscard]] std::string_view name() const noexcept { return m_name; }
    [[nodiscard]] Level level() const noexcept { return m_level.load(std::memory_order_relaxed); }
    void setLevel(Level level) noexcept { m_level.store(level, std::memory_order_relaxed); }

    // True if a message of this level would be written.
    [[nodiscard]] bool shouldLog(Level level) const noexcept {
        const Level minimum = this->level();
        return minimum != Level::Off && level >= minimum;
    }

private:
    std::string_view m_name;
    std::atomic<Level> m_level;
};

// Engine categories. constinit guarantees they exist before any code runs, so
// even static initializers may log. Modules add their own categories here as
// they are built.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): levels change at runtime.
inline constinit Category core{"core", Level::Info};
inline constinit Category assertion{"assert", Level::Trace};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Makes a category findable by name, for the console and the settings system.
// Registering the same category twice has no effect. The category must outlive
// the program's use of the registry (globals always do).
void registerCategory(Category& category);

// Returns the registered category with this name, or nullptr.
[[nodiscard]] Category* findCategory(std::string_view name);

// Returns every registered category, in registration order.
[[nodiscard]] std::vector<Category*> categories();

struct Config {
    // Where the log file is written. The folder is created if needed and the
    // file is replaced on every start. An empty path disables the file.
    std::filesystem::path filePath = "logs/latest.log";
    bool console = true;
    // Extra filters per output, on top of the category levels. For example a
    // quiet console with a detailed file.
    Level consoleLevel = Level::Trace;
    Level fileLevel = Level::Trace;
};

// Opens the console and file outputs and registers the engine categories.
// Before this is called, messages go to stderr so nothing is lost. Calling it
// again replaces the outputs. Returns false if the log file could not be
// opened (console logging still works in that case).
bool initialize(const Config& config);

// Flushes and closes all outputs. Later messages go to stderr again.
void shutdown();

// Writes all buffered messages to their outputs. Warnings and above are
// flushed automatically.
void flush();

namespace detail {

// Writes an already formatted message. Called by the macros below after the
// level check passed.
void write(const Category& category, Level level, std::string_view message);

// One reusable buffer per thread, so formatting a message does not allocate
// once the buffer has grown to the size of the longest message.
[[nodiscard]] std::string& threadBuffer();

template <typename... Args>
void logFormatted(const Category& category, Level level, std::format_string<Args...> format, Args&&... args) {
    if (!category.shouldLog(level)) {
        return;
    }
    std::string& buffer = threadBuffer();
    buffer.clear();
    std::format_to(std::back_inserter(buffer), format, std::forward<Args>(args)...);
    write(category, level, buffer);
}

} // namespace detail
} // namespace vxe::log

// Macros so a future build option can strip trace and debug messages entirely.
#define VXE_LOG_TRACE(category, ...)                                                                         \
    ::vxe::log::detail::logFormatted(category, ::vxe::log::Level::Trace, __VA_ARGS__)
#define VXE_LOG_DEBUG(category, ...)                                                                         \
    ::vxe::log::detail::logFormatted(category, ::vxe::log::Level::Debug, __VA_ARGS__)
#define VXE_LOG_INFO(category, ...)                                                                          \
    ::vxe::log::detail::logFormatted(category, ::vxe::log::Level::Info, __VA_ARGS__)
#define VXE_LOG_WARN(category, ...)                                                                          \
    ::vxe::log::detail::logFormatted(category, ::vxe::log::Level::Warn, __VA_ARGS__)
#define VXE_LOG_ERROR(category, ...)                                                                         \
    ::vxe::log::detail::logFormatted(category, ::vxe::log::Level::Error, __VA_ARGS__)
#define VXE_LOG_CRITICAL(category, ...)                                                                      \
    ::vxe::log::detail::logFormatted(category, ::vxe::log::Level::Critical, __VA_ARGS__)
