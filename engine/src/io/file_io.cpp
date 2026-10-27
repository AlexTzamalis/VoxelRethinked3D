#include "vxe/io/file_io.hpp"

#include <vxe/core/profile.hpp>

#include <format>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

namespace vxe::io {
namespace {

std::string describe(const std::filesystem::path& path) {
    return path.string();
}

// Runs `deliver` where the caller asked for the completion. `deliver` owns
// the request through a unique_ptr, so it fits a job's capture space.
template <typename Fn>
void dispatchCompletion(jobs::JobSystem& jobs, Completion completion, jobs::Priority priority, Fn&& deliver) {
    switch (completion) {
    case Completion::MainThread:
        jobs.runOnMainThread(std::forward<Fn>(deliver));
        return;
    case Completion::Worker: {
        static_cast<void>(jobs.schedule(std::forward<Fn>(deliver), priority));
        return;
    }
    case Completion::IoThread:
        deliver();
        return;
    }
}

} // namespace

Result<std::vector<std::byte>> readFile(const std::filesystem::path& path) {
    VXE_PROFILE_FUNCTION();
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return Error(ErrorCode::NotFound,
                     std::format("Cannot read '{}': {}", describe(path), error.message()));
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error(ErrorCode::IoError, std::format("Cannot open '{}' for reading", describe(path)));
    }
    std::vector<std::byte> bytes(static_cast<usize>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return Error(ErrorCode::IoError, std::format("Reading '{}' stopped early", describe(path)));
    }
    return bytes;
}

Result<void> writeFile(const std::filesystem::path& path, std::span<const std::byte> data, WriteMode mode) {
    VXE_PROFILE_FUNCTION();
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return Error(ErrorCode::IoError, std::format("Cannot create the folder of '{}': {}",
                                                         describe(path), error.message()));
        }
    }

    std::filesystem::path target = path;
    if (mode == WriteMode::Atomic) {
        target += ".tmp";
    }
    {
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Error(ErrorCode::IoError, std::format("Cannot open '{}' for writing", describe(target)));
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file) {
            return Error(ErrorCode::IoError,
                         std::format("Writing '{}' failed (disk full?)", describe(target)));
        }
    }

    if (mode == WriteMode::Atomic) {
        // rename replaces an existing target in one step on both Windows
        // (MoveFileEx with REPLACE_EXISTING) and Linux (rename(2)).
        std::filesystem::rename(target, path, error);
        if (error) {
            std::error_code ignored;
            std::filesystem::remove(target, ignored);
            return Error(ErrorCode::IoError, std::format("Cannot replace '{}' with the new file: {}",
                                                         describe(path), error.message()));
        }
    }
    return {};
}

void AsyncFileIo::read(std::filesystem::path path, ReadCallback onDone, Completion completion,
                       jobs::Priority priority) {
    struct Request {
        std::filesystem::path path;
        ReadCallback onDone;
        Result<std::vector<std::byte>> result = Error(ErrorCode::Unknown);
    };
    auto request = std::make_unique<Request>(Request{.path = std::move(path), .onDone = std::move(onDone)});

    jobs::JobSystem& jobs = m_jobs;
    static_cast<void>(m_jobs.schedule(
        [&jobs, request = std::move(request), completion, priority]() mutable {
            request->result = readFile(request->path);
            dispatchCompletion(jobs, completion, priority, [request = std::move(request)]() {
                request->onDone(std::move(request->result));
            });
        },
        jobs::JobOptions(priority, jobs::CancelToken{}, jobs::Pool::Io)));
}

void AsyncFileIo::write(std::filesystem::path path, std::vector<std::byte> data, WriteCallback onDone,
                        Completion completion, jobs::Priority priority, WriteMode mode) {
    struct Request {
        std::filesystem::path path;
        std::vector<std::byte> data;
        WriteCallback onDone;
        WriteMode mode = WriteMode::Atomic;
        Result<void> result;
    };
    auto request = std::make_unique<Request>(Request{.path = std::move(path),
                                                     .data = std::move(data),
                                                     .onDone = std::move(onDone),
                                                     .mode = mode,
                                                     .result = {}});

    jobs::JobSystem& jobs = m_jobs;
    static_cast<void>(m_jobs.schedule(
        [&jobs, request = std::move(request), completion, priority]() mutable {
            request->result = writeFile(request->path, request->data, request->mode);
            request->data = {}; // Free the buffer before the callback, it is not needed any more.
            dispatchCompletion(jobs, completion, priority, [request = std::move(request)]() {
                if (request->onDone) {
                    request->onDone(std::move(request->result));
                }
            });
        },
        jobs::JobOptions(priority, jobs::CancelToken{}, jobs::Pool::Io)));
}

} // namespace vxe::io
