#include "user_folder.hpp"

#include <algorithm>
#include <format>
#include <system_error>

namespace vr {
namespace fs = std::filesystem;

namespace {

bool hasEntries(const fs::path& folder) {
    std::error_code error;
    return fs::is_directory(folder, error) &&
           fs::directory_iterator(folder, error) != fs::directory_iterator();
}

bool sameFolder(const fs::path& a, const fs::path& b) {
    std::error_code error;
    return fs::exists(a, error) && fs::exists(b, error) && fs::equivalent(a, b, error);
}

// Copies a folder's contents into another, keeping what is there already.
vxe::Result<vxe::usize> copyTree(const fs::path& from, const fs::path& to) {
    std::error_code error;
    vxe::usize files = 0;
    fs::create_directories(to, error);
    for (auto it = fs::recursive_directory_iterator(from, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        const fs::path target = to / fs::relative(it->path(), from, error);
        if (it->is_directory(error)) {
            fs::create_directories(target, error);
        } else if (!fs::exists(target, error)) {
            fs::copy_file(it->path(), target, error);
            ++files;
        }
        if (error) {
            break;
        }
    }
    if (error) {
        return vxe::Error(vxe::ErrorCode::IoError,
                          std::format("Copying {} to {}: {}", from.string(), to.string(), error.message()));
    }
    return files;
}

} // namespace

fs::path chooseUserFolder(const fs::path& programFolder, const fs::path& roamingFolder, bool portable) {
    std::error_code error;
    if (portable || fs::exists(programFolder / kPortableMarker, error) || roamingFolder.empty()) {
        return {};
    }
    return roamingFolder / kUserFolderName;
}

fs::path userFolderOf(const fs::path& configFolder) {
    fs::path folder = configFolder.lexically_normal();
    if (!folder.has_filename()) {
        folder = folder.parent_path(); // Drops the trailing separator.
    }
    return folder.parent_path();
}

vxe::Result<vxe::usize> copyOldUserFiles(const fs::path& programFolder, const fs::path& userFolder,
                                         std::span<const std::string_view> shippedPacks) {
    if (userFolder.empty() || sameFolder(programFolder, userFolder) ||
        fs::exists(userFolder / "config" / "settings.toml") || hasEntries(userFolder / "saves")) {
        return vxe::usize{0};
    }
    vxe::usize files = 0;
    for (const char* name : {"config", "saves"}) {
        if (hasEntries(programFolder / name)) {
            auto copied = copyTree(programFolder / name, userFolder / name);
            if (!copied) {
                return copied.error();
            }
            files += copied.value();
        }
    }
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(programFolder / "textures", error)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_directory(error) || std::ranges::find(shippedPacks, name) != shippedPacks.end()) {
            continue;
        }
        auto copied = copyTree(entry.path(), userFolder / "resourcepacks" / name);
        if (!copied) {
            return copied.error();
        }
        files += copied.value();
    }
    return files;
}

} // namespace vr
