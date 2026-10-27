// vxe_shader_check: validates GLSL shaders at build time (Section 7, E6.4).
//
// Usage: vxe_shader_check --root <shader root> [-D NAME[=VALUE]]... <shader>...
//
// Each shader path is relative to the root and ends in .vert, .frag or .comp.
// The tool runs the engine's own preprocessor (includes, #version, defines),
// so it checks exactly the text the engine hands the driver at runtime, then
// compiles it with glslang, the Khronos reference compiler. Errors are printed
// as "<file>:<line>: error: <message>" with absolute paths, which IDEs and
// terminals turn into clickable links. The exit code is non zero if any shader
// fails, so a broken shader fails the build.

#include <vxe/rhi/shader_source.hpp>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using vxe::rhi::ShaderStage;

struct Arguments {
    std::filesystem::path root;
    std::vector<vxe::rhi::ShaderDefine> defines;
    std::vector<std::string> shaders;
};

bool parseArguments(int argc, char** argv, Arguments& out) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--root" && i + 1 < args.size()) {
            out.root = std::filesystem::path(args[++i]);
        } else if (arg == "-D" && i + 1 < args.size()) {
            const std::string_view define = args[++i];
            const std::size_t equals = define.find('=');
            out.defines.push_back({.name = std::string(define.substr(0, equals)),
                                   .value = equals == std::string_view::npos
                                                ? std::string()
                                                : std::string(define.substr(equals + 1))});
        } else if (arg.starts_with("-")) {
            return false;
        } else {
            out.shaders.emplace_back(arg);
        }
    }
    return !out.root.empty() && !out.shaders.empty();
}

EShLanguage glslangStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return EShLangVertex;
    case ShaderStage::Fragment:
        return EShLangFragment;
    case ShaderStage::Compute:
        return EShLangCompute;
    }
    return EShLangVertex;
}

// Checks one shader. Prints problems to stderr and returns false on failure.
bool checkShader(const Arguments& args, const std::string& path) {
    const std::filesystem::path absolute = std::filesystem::absolute(args.root / path);
    const auto stage = vxe::rhi::shaderStageFromPath(path);
    if (!stage) {
        std::cerr << std::format("{}: error: unknown shader stage; use .vert, .frag or .comp\n",
                                 absolute.generic_string());
        return false;
    }
    const vxe::rhi::ShaderFileReader reader = vxe::rhi::folderShaderReader(args.root);
    const vxe::rhi::ShaderPreprocessOptions options{.stage = *stage, .defines = args.defines};
    const auto preprocessed = vxe::rhi::preprocessShader(path, options, reader);
    if (!preprocessed) {
        // Preprocessor messages start with "<file>:<line>: ", relative to the root.
        std::cerr << std::format("{}/{}\n", std::filesystem::absolute(args.root).generic_string(),
                                 preprocessed.error().message());
        return false;
    }

    std::vector<std::string> files;
    for (const std::string& file : preprocessed.value().files) {
        files.push_back(std::filesystem::absolute(args.root / file).generic_string());
    }

    glslang::TShader shader(glslangStage(*stage));
    const char* text = preprocessed.value().source.c_str();
    const int length = static_cast<int>(preprocessed.value().source.size());
    shader.setStringsWithLengths(&text, &length, 1);
    // No client API is set on purpose: that checks plain OpenGL GLSL as the
    // driver sees it, instead of the stricter SPIR-V for OpenGL rules.
    const bool ok = shader.parse(GetDefaultResources(), 460, ECoreProfile, false, false, EShMsgDefault);
    const std::string log = vxe::rhi::annotateShaderLog(shader.getInfoLog(), files);
    if (!ok) {
        std::cerr << log;
        return false;
    }
    if (!log.empty()) {
        std::cerr << log; // Warnings do not fail the build, but stay visible.
    }
    return true;
}

int run(int argc, char** argv) {
    Arguments args;
    if (!parseArguments(argc, argv, args)) {
        std::cerr << "Usage: vxe_shader_check --root <shader root> [-D NAME[=VALUE]]... <shader>...\n";
        return 2;
    }
    glslang::InitializeProcess();
    bool allOk = true;
    for (const std::string& shader : args.shaders) {
        allOk = checkShader(args, shader) && allOk;
    }
    glslang::FinalizeProcess();
    return allOk ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        // Nothing here throws on purpose; this turns an unexpected failure
        // (out of memory, a broken file system) into a clean build error.
        // fputs, because streams can throw again while reporting.
        std::fputs("vxe_shader_check: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
    } catch (...) {
        std::fputs("vxe_shader_check: unknown failure\n", stderr);
    }
    return 3;
}
