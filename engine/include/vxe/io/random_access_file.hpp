#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>

namespace vxe::io {

enum class OpenMode : u8 {
    ReadWrite,         // The file must exist.
    ReadWriteOrCreate, // Created empty (with missing parent folders) if it does not exist.
    ReadOnly,          // The file must exist; writes fail.
};

// A file read and written at explicit offsets, for formats that update parts
// of a file in place (region files, Section 21). Every read and write names
// its offset, so there is no shared file position and several threads may
// read at the same time while one writes elsewhere in the file.
//
// sync() asks the operating system to put everything written so far on the
// disk (FlushFileBuffers, fsync). Crash safe formats call it between writing
// new data and writing what points to it, so a power loss can never leave a
// pointer to data that did not reach the disk.
//
// Thread safety: reads and writes from several threads are safe as long as
// they do not touch the same bytes at the same time.
class RandomAccessFile {
public:
    [[nodiscard]] static Result<std::unique_ptr<RandomAccessFile>> open(const std::filesystem::path& path,
                                                                        OpenMode mode);
    ~RandomAccessFile();
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;
    RandomAccessFile(RandomAccessFile&&) = delete;
    RandomAccessFile& operator=(RandomAccessFile&&) = delete;

    // Reads exactly out.size() bytes at `offset`. Fails if the file ends first.
    [[nodiscard]] Result<void> read(u64 offset, std::span<std::byte> out) const;
    // Writes all of `data` at `offset`, growing the file if needed.
    [[nodiscard]] Result<void> write(u64 offset, std::span<const std::byte> data);
    [[nodiscard]] Result<void> sync();
    [[nodiscard]] Result<u64> size() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    RandomAccessFile(std::filesystem::path path, std::intptr_t handle) noexcept
        : m_path(std::move(path))
        , m_handle(handle) {}

    std::filesystem::path m_path;
    std::intptr_t m_handle; // A HANDLE on Windows, a file descriptor elsewhere.
};

} // namespace vxe::io
