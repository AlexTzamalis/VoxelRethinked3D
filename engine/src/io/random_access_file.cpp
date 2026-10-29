#include "vxe/io/random_access_file.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace vxe::io {
namespace {

// The largest single read or write call; bigger requests loop. Windows
// takes a DWORD count, so this stays well below 4 GB on every system.
constexpr usize kMaxChunk = usize{1} << 30U;

Error systemError(const std::filesystem::path& path, std::string_view what) {
#ifdef _WIN32
    const auto code = static_cast<int>(GetLastError());
#else
    const int code = errno;
#endif
    return Error(ErrorCode::IoError, std::format("{} '{}' failed: {}", what, path.string(),
                                                 std::system_category().message(code)));
}

#ifdef _WIN32
// The member keeps the HANDLE as an integer so the header needs no Windows
// types; it always holds a value that came from a HANDLE.
HANDLE nativeHandle(std::intptr_t value) noexcept {
    return reinterpret_cast<HANDLE>(value); // NOLINT(performance-no-int-to-ptr)
}
#endif

} // namespace

#ifdef _WIN32

Result<std::unique_ptr<RandomAccessFile>> RandomAccessFile::open(const std::filesystem::path& path,
                                                                 OpenMode mode) {
    if (mode == OpenMode::ReadWriteOrCreate && path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    const DWORD access = mode == OpenMode::ReadOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    const DWORD disposition = mode == OpenMode::ReadWriteOrCreate ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disposition,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Read before formatting the message, which may change the last error.
        const DWORD code = GetLastError();
        Error error = systemError(path, "Opening");
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return Error(ErrorCode::NotFound, error.message());
        }
        return error;
    }
    return std::unique_ptr<RandomAccessFile>(
        new RandomAccessFile(path, reinterpret_cast<std::intptr_t>(handle)));
}

RandomAccessFile::~RandomAccessFile() {
    CloseHandle(nativeHandle(m_handle));
}

Result<void> RandomAccessFile::read(u64 offset, std::span<std::byte> out) const {
    while (!out.empty()) {
        const auto count = static_cast<DWORD>(std::min(out.size(), kMaxChunk));
        // An OVERLAPPED with an offset on a synchronous handle reads at that
        // offset without using (or moving) a shared file position.
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        DWORD done = 0;
        if (ReadFile(nativeHandle(m_handle), out.data(), count, &done, &at) == 0) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                return Error(ErrorCode::IoError, std::format("'{}' ends before the data", m_path.string()));
            }
            return systemError(m_path, "Reading");
        }
        if (done == 0) {
            return Error(ErrorCode::IoError, std::format("'{}' ends before the data", m_path.string()));
        }
        out = out.subspan(done);
        offset += done;
    }
    return {};
}

Result<void> RandomAccessFile::write(u64 offset, std::span<const std::byte> data) {
    while (!data.empty()) {
        const auto count = static_cast<DWORD>(std::min(data.size(), kMaxChunk));
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        DWORD done = 0;
        if (WriteFile(nativeHandle(m_handle), data.data(), count, &done, &at) == 0 || done == 0) {
            return systemError(m_path, "Writing");
        }
        data = data.subspan(done);
        offset += done;
    }
    return {};
}

Result<void> RandomAccessFile::sync() {
    if (FlushFileBuffers(nativeHandle(m_handle)) == 0) {
        return systemError(m_path, "Flushing");
    }
    return {};
}

Result<u64> RandomAccessFile::size() const {
    LARGE_INTEGER bytes{};
    if (GetFileSizeEx(nativeHandle(m_handle), &bytes) == 0) {
        return systemError(m_path, "Measuring");
    }
    return static_cast<u64>(bytes.QuadPart);
}

#else

Result<std::unique_ptr<RandomAccessFile>> RandomAccessFile::open(const std::filesystem::path& path,
                                                                 OpenMode mode) {
    if (mode == OpenMode::ReadWriteOrCreate && path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    int flags = mode == OpenMode::ReadOnly ? O_RDONLY : O_RDWR;
    if (mode == OpenMode::ReadWriteOrCreate) {
        flags |= O_CREAT;
    }
    flags |= O_CLOEXEC;
    const int descriptor = ::open(path.c_str(), flags, 0644); // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (descriptor < 0) {
        // Read before formatting the message, which may change errno.
        const int code = errno;
        Error error = systemError(path, "Opening");
        if (code == ENOENT) {
            return Error(ErrorCode::NotFound, error.message());
        }
        return error;
    }
    return std::unique_ptr<RandomAccessFile>(new RandomAccessFile(path, descriptor));
}

RandomAccessFile::~RandomAccessFile() {
    ::close(static_cast<int>(m_handle));
}

Result<void> RandomAccessFile::read(u64 offset, std::span<std::byte> out) const {
    while (!out.empty()) {
        const ssize_t done = ::pread(static_cast<int>(m_handle), out.data(), std::min(out.size(), kMaxChunk),
                                     static_cast<off_t>(offset));
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return systemError(m_path, "Reading");
        }
        if (done == 0) {
            return Error(ErrorCode::IoError, std::format("'{}' ends before the data", m_path.string()));
        }
        out = out.subspan(static_cast<usize>(done));
        offset += static_cast<u64>(done);
    }
    return {};
}

Result<void> RandomAccessFile::write(u64 offset, std::span<const std::byte> data) {
    while (!data.empty()) {
        const ssize_t done = ::pwrite(static_cast<int>(m_handle), data.data(),
                                      std::min(data.size(), kMaxChunk), static_cast<off_t>(offset));
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return systemError(m_path, "Writing");
        }
        data = data.subspan(static_cast<usize>(done));
        offset += static_cast<u64>(done);
    }
    return {};
}

Result<void> RandomAccessFile::sync() {
    if (::fsync(static_cast<int>(m_handle)) != 0) {
        return systemError(m_path, "Flushing");
    }
    return {};
}

Result<u64> RandomAccessFile::size() const {
    struct stat info{};
    if (::fstat(static_cast<int>(m_handle), &info) != 0) {
        return systemError(m_path, "Measuring");
    }
    return static_cast<u64>(info.st_size);
}

#endif

} // namespace vxe::io
