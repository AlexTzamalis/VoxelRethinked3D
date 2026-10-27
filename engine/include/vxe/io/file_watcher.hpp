#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/time.hpp>
#include <vxe/core/types.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace vxe::io {

enum class FileChange : u8 {
    Added,
    Modified,
    Removed,
    Renamed, // `path` is the new name.
};

struct FileEvent {
    std::filesystem::path path; // Absolute path of the file that changed.
    FileChange change = FileChange::Modified;
};

using WatchId = u64;
using FileEventCallback = std::function<void(const FileEvent&)>;

// Watches folders for changes and reports them on the thread that calls
// poll(), which is the main thread in the engine. Used for hot reloading
// shaders, texture packs and data files (Sections 12.5 and 15.3).
//
// The operating system reports changes on a background thread, often several
// per save (an editor may write a file, truncate it and write it again).
// Events are therefore collected per file and only delivered once that file
// has been quiet for the debounce time, as one event with the latest change.
// That turns a burst of writes into a single reload.
//
// Thread safety: watch(), unwatch() and poll() belong to one thread (the one
// that created the watcher). Callbacks run inside poll() on that thread.
class FileWatcher {
public:
    explicit FileWatcher(Duration debounce = std::chrono::milliseconds(200));
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) = delete;
    FileWatcher& operator=(FileWatcher&&) = delete;

    // Starts watching `folder` (and its sub folders when `recursive`).
    // `callback` receives every debounced change below it.
    [[nodiscard]] Result<WatchId> watch(const std::filesystem::path& folder, bool recursive,
                                        FileEventCallback callback);

    // Stops a watch. Pending events of that watch are dropped.
    void unwatch(WatchId id);

    // Delivers every event whose file has been quiet for the debounce time,
    // and returns how many were delivered. Call once per frame.
    usize poll();

    // Same as poll(), but treats `now` as the current time. For tests.
    usize poll(TimePoint now);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vxe::io
