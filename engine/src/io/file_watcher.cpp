#include "vxe/io/file_watcher.hpp"

#include <vxe/core/containers.hpp>
#include <vxe/core/log.hpp>

#include <efsw/efsw.hpp>

#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vxe::io {
namespace {

FileChange toFileChange(efsw::Action action) noexcept {
    switch (action) {
    case efsw::Actions::Add:
        return FileChange::Added;
    case efsw::Actions::Delete:
        return FileChange::Removed;
    case efsw::Actions::Moved:
        return FileChange::Renamed;
    case efsw::Actions::Modified:
        break;
    }
    return FileChange::Modified;
}

} // namespace

// efsw calls handleFileAction on its own thread, so everything shared with
// poll() sits behind `mutex`.
struct FileWatcher::Impl final : efsw::FileWatchListener {
    struct Watch {
        FileEventCallback callback;
    };

    struct Pending {
        WatchId watch = 0;
        FileEvent event;
        TimePoint lastChange;
    };

    explicit Impl(Duration debounceTime)
        : debounce(debounceTime) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() override = default;

    void handleFileAction(efsw::WatchID efswId, const std::string& directory, const std::string& filename,
                          efsw::Action action, const std::string& /*oldFilename*/) override {
        const std::filesystem::path path = std::filesystem::path(directory) / filename;
        const std::scoped_lock lock(mutex);
        const auto watch = byEfswId.find(efswId);
        if (watch == byEfswId.end()) {
            return; // Unwatched while the event was in flight.
        }
        // One pending entry per file: later changes overwrite earlier ones and
        // restart the quiet period.
        Pending& entry = pending[path.lexically_normal().string()];
        entry.watch = watch->second;
        entry.event = FileEvent{.path = path.lexically_normal(), .change = toFileChange(action)};
        entry.lastChange = Clock::now();
    }

    Duration debounce;
    bool started = false;

    std::mutex mutex;
    HashMap<WatchId, Watch> watches; // Only touched on the owner thread, plus under `mutex` for ids.
    HashMap<efsw::WatchID, WatchId> byEfswId;
    HashMap<WatchId, efsw::WatchID> toEfswId;
    StringMap<Pending> pending;
    WatchId nextId = 1;
    std::vector<Pending> ready; // Reused by poll() to avoid reallocating.

    // Declared last so it is destroyed first: its destructor stops efsw's
    // thread, which calls handleFileAction and uses the members above.
    efsw::FileWatcher backend;
};

FileWatcher::FileWatcher(Duration debounce)
    : m_impl(std::make_unique<Impl>(debounce)) {}

FileWatcher::~FileWatcher() = default;

Result<WatchId> FileWatcher::watch(const std::filesystem::path& folder, bool recursive,
                                   FileEventCallback callback) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(folder, error).lexically_normal();
    if (error || !std::filesystem::is_directory(absolute, error)) {
        return Error(ErrorCode::NotFound, std::format("Cannot watch '{}': not a folder", folder.string()));
    }

    // Linux has no recursive watches, so efsw adds one per sub folder as they
    // appear. Files created in a new folder before its watch exists would be
    // missed (copying a whole texture pack into textures/ does exactly that).
    // Synthetic events report the files already there once the watch is
    // added; the duplicates this can cause are merged by the debounce.
    const std::vector<efsw::WatcherOption> options{
        efsw::WatcherOption(efsw::Options::LinuxProduceSyntheticEvents, 1)};
    const efsw::WatchID efswId =
        m_impl->backend.addWatch(absolute.string(), m_impl.get(), recursive, options);
    if (efswId < 0) {
        return Error(ErrorCode::IoError, std::format("Cannot watch '{}': {}", folder.string(),
                                                     efsw::Errors::Log::getLastErrorLog()));
    }

    WatchId id = 0;
    {
        const std::scoped_lock lock(m_impl->mutex);
        id = m_impl->nextId++;
        m_impl->watches.emplace(id, Impl::Watch{.callback = std::move(callback)});
        m_impl->byEfswId.emplace(efswId, id);
        m_impl->toEfswId.emplace(id, efswId);
    }
    if (!m_impl->started) {
        // efsw's thread starts with the first watch, so an idle watcher costs nothing.
        m_impl->backend.watch();
        m_impl->started = true;
    }
    return id;
}

void FileWatcher::unwatch(WatchId id) {
    efsw::WatchID efswId = 0;
    {
        const std::scoped_lock lock(m_impl->mutex);
        const auto found = m_impl->toEfswId.find(id);
        if (found == m_impl->toEfswId.end()) {
            return;
        }
        efswId = found->second;
        m_impl->toEfswId.erase(found);
        m_impl->byEfswId.erase(efswId);
        m_impl->watches.erase(id);
        for (auto it = m_impl->pending.begin(); it != m_impl->pending.end();) {
            it = it->second.watch == id ? m_impl->pending.erase(it) : std::next(it);
        }
    }
    m_impl->backend.removeWatch(efswId);
}

usize FileWatcher::poll() {
    return poll(Clock::now());
}

usize FileWatcher::poll(TimePoint now) {
    std::vector<Impl::Pending>& ready = m_impl->ready;
    ready.clear();
    {
        const std::scoped_lock lock(m_impl->mutex);
        for (auto it = m_impl->pending.begin(); it != m_impl->pending.end();) {
            if (now - it->second.lastChange >= m_impl->debounce) {
                ready.push_back(std::move(it->second));
                it = m_impl->pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Callbacks run without the lock, so they may watch, unwatch or take
    // their time without blocking efsw's thread.
    usize delivered = 0;
    for (const Impl::Pending& entry : ready) {
        const auto watch = m_impl->watches.find(entry.watch);
        if (watch != m_impl->watches.end() && watch->second.callback) {
            watch->second.callback(entry.event);
            ++delivered;
        }
    }
    return delivered;
}

} // namespace vxe::io
