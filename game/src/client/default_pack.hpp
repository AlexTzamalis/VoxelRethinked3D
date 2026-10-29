#pragma once

#include <vxe/render/texture_pack.hpp>

#include <span>

namespace vr {

// The files of the default texture pack (textures/default), built into the
// program by cmake/EmbedFiles.cmake, so every block texture resolves even
// without a textures/ folder (Section 15.1). For render::openEmbeddedPack.
[[nodiscard]] std::span<const vxe::render::EmbeddedFile> defaultPackFiles();

} // namespace vr
