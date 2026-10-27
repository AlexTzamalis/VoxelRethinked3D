#include "vxe/io/vfs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <mutex>
#include <system_error>

namespace vxe::io {
namespace {

constexpr usize kMaxMountNameLength = 32;

bool isValidMountName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxMountNameLength) {
        return false;
    }
    return std::ranges::all_of(
        name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

char toLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Windows treats these names as devices in every folder, with or without an
// extension ("nul.txt" is still the null device). Rejecting them everywhere
// keeps saves portable.
bool isReservedDeviceName(std::string_view part) noexcept {
    const std::string_view stem = part.substr(0, part.find('.'));
    std::array<char, 8> lower{};
    if (stem.size() > lower.size()) {
        return false;
    }
    std::ranges::transform(stem, lower.begin(), toLowerAscii);
    const std::string_view name(lower.data(), stem.size());

    constexpr std::array<std::string_view, 6> kFixed{"con", "prn", "aux", "nul", "conin$", "conout$"};
    if (std::ranges::find(kFixed, name) != kFixed.end()) {
        return true;
    }
    // COM1 to COM9 and LPT1 to LPT9.
    return name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '1' &&
           name[3] <= '9';
}

bool isForbiddenCharacter(char c) noexcept {
    const auto code = static_cast<unsigned char>(c);
    if (code < 0x20 || code == 0x7F) {
        return true;
    }
    constexpr std::string_view kForbidden = "<>\"|?*:";
    return kForbidden.find(c) != std::string_view::npos;
}

Error invalidPath(std::string_view path, std::string_view reason) {
    return Error(ErrorCode::InvalidArgument, std::format("Invalid path '{}': {}", path, reason));
}

} // namespace

Result<std::string> normalizeRelativePath(std::string_view path) {
    if (path.starts_with('/') || path.starts_with('\\')) {
        return invalidPath(path, "absolute paths are not allowed");
    }

    std::vector<std::string_view> parts;
    usize start = 0;
    while (start <= path.size()) {
        usize end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        start = end + 1;

        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (parts.empty()) {
                return invalidPath(path, "it climbs above the mount root");
            }
            parts.pop_back();
            continue;
        }
        if (std::ranges::any_of(part, isForbiddenCharacter)) {
            return invalidPath(path, "it contains a character that is not allowed in file names");
        }
        if (part.back() == '.' || part.back() == ' ') {
            return invalidPath(path, "a name ends with a dot or a space");
        }
        if (isReservedDeviceName(part)) {
            return invalidPath(path, "it uses a reserved device name");
        }
        parts.push_back(part);
    }

    std::string normalized;
    for (const std::string_view part : parts) {
        if (!normalized.empty()) {
            normalized += '/';
        }
        normalized += part;
    }
    return normalized;
}

Result<void> Vfs::mount(std::string_view name, const std::filesystem::path& root,
                        const MountOptions& options) {
    if (!isValidMountName(name)) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("Invalid mount name '{}': use 1 to 32 characters of a-z, 0-9 and _", name));
    }

    std::error_code error;
    if (options.createIfMissing) {
        std::filesystem::create_directories(root, error);
        if (error) {
            return Error(ErrorCode::IoError, std::format("Cannot create folder '{}' for mount '{}': {}",
                                                         root.string(), name, error.message()));
        }
    }
    if (!std::filesystem::is_directory(root, error)) {
        return Error(
            ErrorCode::NotFound,
            std::format("Mount '{}' needs an existing folder, but '{}' is not one", name, root.string()));
    }

    // Absolute and normalized once here, so resolve() only appends.
    std::filesystem::path absolute = std::filesystem::absolute(root, error).lexically_normal();
    if (error) {
        return Error(ErrorCode::IoError,
                     std::format("Cannot make '{}' absolute: {}", root.string(), error.message()));
    }

    const std::unique_lock lock(m_mutex);
    m_mounts.insert_or_assign(std::string(name),
                              Mount{.root = std::move(absolute), .access = options.access});
    return {};
}

bool Vfs::unmount(std::string_view name) {
    const std::unique_lock lock(m_mutex);
    const auto found = m_mounts.find(name);
    if (found == m_mounts.end()) {
        return false;
    }
    m_mounts.erase(found);
    return true;
}

bool Vfs::isMounted(std::string_view name) const {
    const std::shared_lock lock(m_mutex);
    return m_mounts.contains(name);
}

Result<std::filesystem::path> Vfs::resolveWithAccess(std::string_view virtualPath, bool forWrite) const {
    constexpr std::string_view kSeparator = ":/";
    const usize separator = virtualPath.find(kSeparator);
    if (separator == std::string_view::npos) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("'{}' is not a virtual path; expected 'mount:/path'", virtualPath));
    }
    const std::string_view mountName = virtualPath.substr(0, separator);
    const std::string_view inside = virtualPath.substr(separator + kSeparator.size());

    Result<std::string> relative = normalizeRelativePath(inside);
    if (!relative) {
        return std::move(relative).error();
    }

    const std::shared_lock lock(m_mutex);
    const auto found = m_mounts.find(mountName);
    if (found == m_mounts.end()) {
        return Error(ErrorCode::NotFound,
                     std::format("No mount named '{}' (in '{}')", mountName, virtualPath));
    }
    if (forWrite && found->second.access == MountAccess::ReadOnly) {
        return Error(ErrorCode::PermissionDenied, std::format("Mount '{}' is read only", mountName));
    }
    if (relative.value().empty()) {
        return found->second.root;
    }
    return found->second.root / std::filesystem::path(relative.value());
}

Result<std::filesystem::path> Vfs::resolve(std::string_view virtualPath) const {
    return resolveWithAccess(virtualPath, false);
}

Result<std::filesystem::path> Vfs::resolveForWrite(std::string_view virtualPath) const {
    return resolveWithAccess(virtualPath, true);
}

bool Vfs::exists(std::string_view virtualPath) const {
    const Result<std::filesystem::path> path = resolve(virtualPath);
    std::error_code error;
    return path.hasValue() && std::filesystem::exists(path.value(), error);
}

Result<std::vector<std::string>> Vfs::list(std::string_view virtualFolder) const {
    Result<std::filesystem::path> folder = resolve(virtualFolder);
    if (!folder) {
        return std::move(folder).error();
    }
    std::error_code error;
    std::filesystem::directory_iterator iterator(folder.value(), error);
    if (error) {
        return Error(ErrorCode::NotFound,
                     std::format("Cannot list '{}': {}", virtualFolder, error.message()));
    }
    // The non throwing increment, because engine code does not use exceptions.
    std::vector<std::string> names;
    for (; !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        names.push_back(iterator->path().filename().string());
    }
    if (error) {
        return Error(ErrorCode::IoError,
                     std::format("Listing '{}' failed: {}", virtualFolder, error.message()));
    }
    std::ranges::sort(names);
    return names;
}

} // namespace vxe::io
