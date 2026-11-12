#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace vxe::io {

// Packs every file under `folder` into a new zip file at `zipPath` (replaced
// if it exists), each stored as `rootName/<path inside folder>`. Folders named
// in `skipFolders` are left out wherever they appear (caches that rebuild
// themselves). Files are compressed one at a time, so memory stays at the
// size of the largest file. Returns the number of files packed.
//
// Thread safety: any thread; blocking, so call it from an IO job.
[[nodiscard]] Result<usize> zipFolder(const std::filesystem::path& folder,
                                      const std::filesystem::path& zipPath, std::string_view rootName,
                                      std::span<const std::string_view> skipFolders = {});

// Unpacks a zip file into `destination` (created when missing). An entry that
// would land outside it (an absolute path, a drive letter, a ".." step) fails
// the whole unpack before anything is written, so a crafted archive cannot
// touch other files. Returns the number of files written.
//
// Thread safety: any thread; blocking, so call it from an IO job.
[[nodiscard]] Result<usize> unzipTo(const std::filesystem::path& zipPath,
                                    const std::filesystem::path& destination);

// The relative path an entry name stands for inside the destination, or an
// empty path when the name could escape it. Backslashes count as separators.
[[nodiscard]] std::filesystem::path safeZipEntryPath(std::string_view name);

} // namespace vxe::io
