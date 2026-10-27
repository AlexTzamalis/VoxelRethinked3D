#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/jobs/job_system.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace vxe::io {

// Reads a whole file. Blocks, so never call it on the main thread for files
// that may be large; use AsyncFileIo instead.
[[nodiscard]] Result<std::vector<std::byte>> readFile(const std::filesystem::path& path);

// How writeFile replaces an existing file.
enum class WriteMode : u8 {
    // Write to "<file>.tmp", then rename it over the target. A crash or power
    // loss leaves either the old file or the new one, never half of each
    // (Section 21 crash safety). The default for anything the player keeps.
    Atomic,
    // Write the target directly. Faster, for scratch files and logs.
    Direct,
};

// Writes a whole file, creating missing parent folders. Blocks like readFile.
[[nodiscard]] Result<void> writeFile(const std::filesystem::path& path, std::span<const std::byte> data,
                                     WriteMode mode = WriteMode::Atomic);

// Where a completion callback runs.
enum class Completion : u8 {
    MainThread, // Queued with JobSystem::runOnMainThread (UI, GPU uploads).
    Worker,     // Scheduled as a compute job (decompress, parse).
    IoThread,   // Called right away on the IO thread. Keep it tiny.
};

using ReadCallback = std::function<void(Result<std::vector<std::byte>>&&)>;
using WriteCallback = std::function<void(Result<void>&&)>;

// Runs file reads and writes on the job system's IO threads, so disk access
// never blocks the main thread or a compute worker (Section 0.2), and hands
// the result to a callback where the caller wants it.
//
// Each request allocates a small record for the path, data and callback. File
// operations are far rarer than jobs, so that is fine. Callbacks are
// std::function, so they must be copyable; a callback that needs to own
// something move only can hold it through a std::shared_ptr.
//
// Thread safety: any thread may start requests. The JobSystem must outlive
// all requests (its destructor finishes them).
class AsyncFileIo {
public:
    explicit AsyncFileIo(jobs::JobSystem& jobs) noexcept
        : m_jobs(jobs) {}

    void read(std::filesystem::path path, ReadCallback onDone, Completion completion = Completion::MainThread,
              jobs::Priority priority = jobs::Priority::Normal);

    void write(std::filesystem::path path, std::vector<std::byte> data, WriteCallback onDone,
               Completion completion = Completion::MainThread,
               jobs::Priority priority = jobs::Priority::Normal, WriteMode mode = WriteMode::Atomic);

private:
    jobs::JobSystem& m_jobs;
};

} // namespace vxe::io
