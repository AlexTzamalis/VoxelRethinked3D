#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>
#include <vxe/rhi/types.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// GLSL source preparation shared by every backend and the build time shader
// checker (Section 7.3, E6.4).
//
// OpenGL drivers do not understand #include, so the engine resolves includes
// itself before handing source to the driver. The build time checker
// (tools/shader_check) runs the very same code, so a shader that passes the
// build compiles at runtime with exactly the same text.
namespace vxe::rhi {

// Reads one shader file, given its path relative to the shader root (for
// example "include/bindings.glsl"). Returns the file's text.
using ShaderFileReader = std::function<Result<std::string>(std::string_view path)>;

struct ShaderDefine {
    std::string name{};
    std::string value{}; // Empty for a plain "#define NAME".
};

struct ShaderPreprocessOptions {
    ShaderStage stage = ShaderStage::Vertex;
    Backend backend = Backend::OpenGL;
    u32 glslVersion = 460;
    std::vector<ShaderDefine> defines{};
};

struct PreprocessedShader {
    // Complete source, ready for ShaderDesc::source.
    std::string source;
    // files[i] is the file behind source string number i in the #line
    // directives. Pass it as ShaderDesc::fileNames for readable errors.
    std::vector<std::string> files;
};

// Builds the full source of the shader at `path`:
//   #version <glslVersion> core
//   #define VXE_OPENGL 1             (VXE_VULKAN for a future Vulkan backend)
//   #define VXE_STAGE_VERTEX 1       (or _FRAGMENT, _COMPUTE)
//   #define <each option define>
//   the file, with every   #include "path"   replaced by that file.
//
// Include paths are relative to the shader root, never to the including
// file, so a file can be moved without editing what it includes. A file that
// starts with "#pragma once" is included only the first time. Shader files
// must not contain #version: the engine writes it, because the version
// depends on the device. #line directives keep every line traceable to its
// file, so driver errors point at the right place.
[[nodiscard]] Result<PreprocessedShader> preprocessShader(std::string_view path,
                                                          const ShaderPreprocessOptions& options,
                                                          const ShaderFileReader& reader);

// The stage a shader file holds, from its extension: .vert, .frag or .comp.
// Include files (.glsl) have no stage.
[[nodiscard]] std::optional<ShaderStage> shaderStageFromPath(std::string_view path) noexcept;

// "vertex", "fragment" or "compute", for messages.
[[nodiscard]] std::string_view nameOf(ShaderStage stage) noexcept;

// Rewrites a compiler log so positions name files: "0(12) : error C1008"
// (NVIDIA), "ERROR: 0:12: ..." (AMD and glslang) and "0:12(5): error: ..."
// (Mesa) all become "<files[0]>:12: error ...". Lines without a position
// are kept as they are. Source numbers outside `files` become "<source N>".
[[nodiscard]] std::string annotateShaderLog(std::string_view log, std::span<const std::string> files);

} // namespace vxe::rhi
