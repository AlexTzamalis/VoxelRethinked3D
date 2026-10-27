#pragma once

#include <vxe/core/containers.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <filesystem>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace vxe::io {

// What a mount allows.
enum class MountAccess : u8 {
    ReadOnly,  // assets:/ and textures:/ are never written by the engine.
    ReadWrite, // config:/ and saves:/.
};

struct MountOptions {
    MountAccess access = MountAccess::ReadOnly;
    // Create the folder if it does not exist yet (for config:/ and saves:/ on
    // first start). Without this, mounting a missing folder fails.
    bool createIfMissing = false;
};

// Turns a path inside a mount into a clean, safe relative path, or explains
// why it is not acceptable. Pure string work, no file system access.
//
// The result uses "/" separators, has no empty, "." or ".." parts and never
// starts with "/". Rejected (ErrorCode::InvalidArgument):
//   - ".." that would climb above the mount root ("../x", "a/../../x"),
//   - absolute paths and drive letters ("/x", "C:/x", "\\server\share"),
//   - ":" anywhere (drive letters, and NTFS alternate data streams),
//   - control characters and the characters < > " | ? *,
//   - Windows device names in any part ("con", "nul.txt", "COM1"),
//   - parts ending in "." or " " (Windows silently strips them, so "save."
//     and "save" would be the same file).
// The rules are the same on every platform, so a save that works on Linux
// also works on Windows.
[[nodiscard]] Result<std::string> normalizeRelativePath(std::string_view path);

// The virtual file system: named mount points over real folders, so engine
// code says "saves:/MyWorld/world.toml" and never builds real paths itself
// (Section 4 runtime layout, E4.1).
//
// Every lookup goes through normalizeRelativePath, so a world name typed by a
// player or a path from a texture pack can never reach outside its mount.
//
// Thread safety: all functions may be called from any thread. Mounting takes
// an exclusive lock, lookups a shared one, so worker threads resolving paths
// never block each other.
class Vfs {
public:
    // Maps `name:/` to the folder `root`. Mount names are 1 to 32 characters
    // of a to z, 0 to 9 and "_". Mounting an existing name replaces it.
    Result<void> mount(std::string_view name, const std::filesystem::path& root,
                       const MountOptions& options = {});

    // Returns false if no such mount existed.
    bool unmount(std::string_view name);

    [[nodiscard]] bool isMounted(std::string_view name) const;

    // The real path for a virtual path such as "saves:/MyWorld/world.toml".
    // Does not check that the file exists. "saves:/" alone is the mount root.
    [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view virtualPath) const;

    // Like resolve(), but fails with PermissionDenied for read only mounts.
    // Every write in the engine goes through this.
    [[nodiscard]] Result<std::filesystem::path> resolveForWrite(std::string_view virtualPath) const;

    [[nodiscard]] bool exists(std::string_view virtualPath) const;

    // Names of the files and folders directly inside a virtual folder,
    // sorted. Hidden system entries are not filtered.
    [[nodiscard]] Result<std::vector<std::string>> list(std::string_view virtualFolder) const;

private:
    struct Mount {
        std::filesystem::path root;
        MountAccess access = MountAccess::ReadOnly;
    };

    [[nodiscard]] Result<std::filesystem::path> resolveWithAccess(std::string_view virtualPath,
                                                                  bool forWrite) const;

    mutable std::shared_mutex m_mutex;
    StringMap<Mount> m_mounts;
};

} // namespace vxe::io
