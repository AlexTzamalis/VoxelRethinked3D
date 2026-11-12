#include "vxe/io/zip.hpp"

#include <vxe/io/file_io.hpp>

#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace vxe::io {
namespace {

namespace fs = std::filesystem;

// miniz opens files by narrow name, which on Windows is not UTF-8; opening
// them here (wide names on Windows) keeps every folder name working.
struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        if (file != nullptr) {
            static_cast<void>(std::fclose(file));
        }
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr openFile(const fs::path& path, bool write) {
#ifdef _WIN32
    std::FILE* file = nullptr;
    static_cast<void>(_wfopen_s(&file, path.c_str(), write ? L"wb" : L"rb"));
    return FilePtr(file);
#else
    return FilePtr(std::fopen(path.c_str(), write ? "wb" : "rb"));
#endif
}

std::string utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string zipError(mz_zip_archive& zip) {
    return mz_zip_get_error_string(mz_zip_get_last_error(&zip));
}

} // namespace

fs::path safeZipEntryPath(std::string_view name) {
    std::string text(name);
    std::ranges::replace(text, '\\', '/');
    if (text.empty() || text.front() == '/' || text.find(':') != std::string::npos) {
        return {};
    }
    fs::path result;
    usize start = 0;
    while (start <= text.size()) {
        const usize slash = std::min(text.find('/', start), text.size());
        const std::string_view part = std::string_view(text).substr(start, slash - start);
        if (part == "..") {
            return {};
        }
        if (!part.empty() && part != ".") {
            result /= fs::path(std::u8string(part.begin(), part.end()));
        }
        start = slash + 1;
    }
    return result;
}

Result<usize> zipFolder(const fs::path& folder, const fs::path& zipPath, std::string_view rootName,
                        std::span<const std::string_view> skipFolders) {
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        return Error(ErrorCode::NotFound, std::format("No folder at {}", folder.string()));
    }
    fs::create_directories(zipPath.parent_path(), error);
    FilePtr file = openFile(zipPath, true);
    if (!file) {
        return Error(ErrorCode::IoError, std::format("Cannot write {}", zipPath.string()));
    }
    mz_zip_archive zip{};
    if (mz_zip_writer_init_cfile(&zip, file.get(), 0) == 0) {
        return Error(ErrorCode::IoError, std::format("Zip {}: {}", zipPath.string(), zipError(zip)));
    }
    usize files = 0;
    std::string problem;
    for (auto it = fs::recursive_directory_iterator(folder, error);
         !error && it != fs::recursive_directory_iterator() && problem.empty(); it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(error)) {
            if (std::ranges::find(skipFolders, name) != skipFolders.end()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const auto bytes = readFile(it->path());
        if (!bytes) {
            problem = bytes.error().message();
            break;
        }
        const std::string entry = std::string(rootName) + "/" + utf8(fs::relative(it->path(), folder, error));
        if (mz_zip_writer_add_mem(&zip, entry.c_str(), bytes.value().data(), bytes.value().size(),
                                  static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)) == 0) {
            problem = std::format("{}: {}", entry, zipError(zip));
            break;
        }
        ++files;
    }
    if (problem.empty() && error) {
        problem = error.message();
    }
    if (problem.empty() && mz_zip_writer_finalize_archive(&zip) == 0) {
        problem = zipError(zip);
    }
    mz_zip_writer_end(&zip);
    file.reset();
    if (!problem.empty()) {
        fs::remove(zipPath, error);
        return Error(ErrorCode::IoError, std::format("Zip {}: {}", zipPath.string(), problem));
    }
    return files;
}

Result<usize> unzipTo(const fs::path& zipPath, const fs::path& destination) {
    std::error_code error;
    const auto size = fs::file_size(zipPath, error);
    FilePtr file = error ? FilePtr() : openFile(zipPath, false);
    if (!file) {
        return Error(ErrorCode::NotFound, std::format("Cannot read {}", zipPath.string()));
    }
    mz_zip_archive zip{};
    if (mz_zip_reader_init_cfile(&zip, file.get(), size, 0) == 0) {
        return Error(ErrorCode::ParseError,
                     std::format("{} is not a zip file: {}", zipPath.string(), zipError(zip)));
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    // Every name is checked before anything is written.
    std::vector<std::pair<mz_uint, fs::path>> entries;
    std::string problem;
    for (mz_uint index = 0; index < count && problem.empty(); ++index) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&zip, index, &stat) == 0) {
            problem = zipError(zip);
        } else if (mz_zip_reader_is_file_a_directory(&zip, index) == 0) {
            fs::path relative = safeZipEntryPath(stat.m_filename);
            if (relative.empty()) {
                problem = std::format("the entry '{}' points outside the folder", stat.m_filename);
            } else {
                entries.emplace_back(index, std::move(relative));
            }
        }
    }
    usize files = 0;
    for (const auto& [index, relative] : entries) {
        if (!problem.empty()) {
            break;
        }
        usize length = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, index, &length, 0);
        if (data == nullptr) {
            problem = std::format("{}: {}", relative.string(), zipError(zip));
            break;
        }
        const auto written =
            writeFile(destination / relative, std::span(static_cast<const std::byte*>(data), length));
        mz_free(data);
        if (!written) {
            problem = written.error().message();
            break;
        }
        ++files;
    }
    mz_zip_reader_end(&zip);
    if (!problem.empty()) {
        return Error(ErrorCode::ParseError, std::format("Unzip {}: {}", zipPath.string(), problem));
    }
    return files;
}

} // namespace vxe::io
