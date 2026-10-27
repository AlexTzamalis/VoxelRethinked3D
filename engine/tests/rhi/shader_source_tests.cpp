#include <vxe/rhi/shader_source.hpp>

#include <doctest/doctest.h>

#include <format>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace vxe::rhi;

namespace {

using FileMap = std::map<std::string, std::string, std::less<>>;

// A reader over in memory files, so the tests need no disk.
ShaderFileReader readerFor(FileMap files) {
    // Shared, because moving a std::map may allocate, which a std::function
    // holding the lambda must not risk.
    return [files = std::make_shared<const FileMap>(std::move(files))](
               std::string_view path) -> vxe::Result<std::string> {
        const auto found = files->find(path);
        if (found == files->end()) {
            return vxe::Error(vxe::ErrorCode::NotFound, std::format("no file '{}'", path));
        }
        return found->second;
    };
}

} // namespace

TEST_CASE("The preprocessor writes the header and expands includes with #line directives") {
    const ShaderFileReader reader = readerFor({
        {"main.vert", "#include \"include/a.glsl\"\nvoid main() {}\n"},
        {"include/a.glsl", "#pragma once\nfloat a() { return 1.0; }\n"},
    });
    const ShaderPreprocessOptions options{.stage = ShaderStage::Vertex,
                                          .defines = {{.name = "FOO", .value = "2"}, {.name = "BAR"}}};
    const auto result = preprocessShader("main.vert", options, reader);
    REQUIRE(result.hasValue());
    CHECK(result.value().source == "#version 460 core\n"
                                   "#define VXE_OPENGL 1\n"
                                   "#define VXE_STAGE_VERTEX 1\n"
                                   "#define FOO 2\n"
                                   "#define BAR \n"
                                   "#line 1 0\n"
                                   "#line 1 1\n"
                                   "\n" // The #pragma once line, kept so line numbers stay right.
                                   "float a() { return 1.0; }\n"
                                   "#line 2 0\n"
                                   "void main() {}\n");
    CHECK(result.value().files == std::vector<std::string>{"main.vert", "include/a.glsl"});
}

TEST_CASE("Stage and backend defines follow the options") {
    const ShaderFileReader reader = readerFor({{"x.comp", "void main() {}\n"}});
    const auto compute = preprocessShader(
        "x.comp",
        {.stage = ShaderStage::Compute, .backend = Backend::Null, .glslVersion = 450, .defines = {}}, reader);
    REQUIRE(compute.hasValue());
    CHECK(compute.value().source.starts_with(
        "#version 450 core\n#define VXE_OPENGL 1\n#define VXE_STAGE_COMPUTE 1\n"));
}

TEST_CASE("#pragma once files are included once, others every time") {
    const ShaderFileReader reader = readerFor({
        {"main.frag",
         "#include \"once.glsl\"\n#include \"once.glsl\"\n#include \"many.glsl\"\n#include \"many.glsl\"\n"},
        {"once.glsl", "#pragma once\nONCE\n"},
        {"many.glsl", "MANY\n"},
    });
    const auto result =
        preprocessShader("main.frag", {.stage = ShaderStage::Fragment, .defines = {}}, reader);
    REQUIRE(result.hasValue());
    const std::string& source = result.value().source;
    const auto count = [&source](std::string_view word) {
        std::size_t found = 0;
        for (std::size_t at = source.find(word); at != std::string::npos; at = source.find(word, at + 1)) {
            ++found;
        }
        return found;
    };
    CHECK(count("ONCE") == 1);
    CHECK(count("MANY") == 2);
    // Each file is listed once even when included twice.
    CHECK(result.value().files == std::vector<std::string>{"main.frag", "once.glsl", "many.glsl"});
}

TEST_CASE("Preprocessor errors name the file and line that caused them") {
    SUBCASE("missing include") {
        const auto result = preprocessShader(
            "main.frag", {}, readerFor({{"main.frag", "// comment\n  #  include \"missing.glsl\"\n"}}));
        REQUIRE(result.hasError());
        CHECK(result.error().code() == vxe::ErrorCode::NotFound);
        CHECK(result.error().message().starts_with("main.frag:2: cannot include 'missing.glsl'"));
    }
    SUBCASE("missing shader") {
        const auto result = preprocessShader("gone.vert", {}, readerFor({}));
        REQUIRE(result.hasError());
        CHECK(result.error().message().starts_with("gone.vert: cannot read: "));
    }
    SUBCASE("include cycle") {
        const auto result = preprocessShader("main.frag", {},
                                             readerFor({{"main.frag", "#include \"a.glsl\"\n"},
                                                        {"a.glsl", "\n#include \"b.glsl\"\n"},
                                                        {"b.glsl", "#include \"a.glsl\"\n"}}));
        REQUIRE(result.hasError());
        CHECK(result.error().message() == "b.glsl:1: include cycle main.frag -> a.glsl -> b.glsl -> a.glsl");
    }
    SUBCASE("#version in a file") {
        const auto result = preprocessShader(
            "main.frag", {}, readerFor({{"main.frag", "\n\n#version 330\nvoid main() {}\n"}}));
        REQUIRE(result.hasError());
        CHECK(result.error().message().starts_with("main.frag:3: remove #version"));
    }
    SUBCASE("angle bracket include") {
        const auto result =
            preprocessShader("main.frag", {}, readerFor({{"main.frag", "#include <a.glsl>\n"}}));
        REQUIRE(result.hasError());
        CHECK(result.error().message().starts_with("main.frag:1: write includes as #include \""));
    }
    SUBCASE("include paths that leave the shader root") {
        for (const std::string_view path :
             {"../x.glsl", "/abs.glsl", "a\\b.glsl", "C:/x.glsl", "a//b.glsl", "a/./../b"}) {
            const auto result = preprocessShader(
                "main.frag", {}, readerFor({{"main.frag", std::format("#include \"{}\"\n", path)}}));
            REQUIRE(result.hasError());
            CHECK(result.error().message().find("must be relative to the shader root") != std::string::npos);
        }
    }
    SUBCASE("words that only start like directives are plain code") {
        const auto result = preprocessShader(
            "main.frag", {}, readerFor({{"main.frag", "#includes_nothing\n#pragma optimize(on)\n"}}));
        REQUIRE(result.hasValue());
        CHECK(result.value().source.ends_with("#includes_nothing\n#pragma optimize(on)\n"));
    }
}

TEST_CASE("Shader stages come from the file extension") {
    CHECK(shaderStageFromPath("debug/triangle.vert") == ShaderStage::Vertex);
    CHECK(shaderStageFromPath("a.frag") == ShaderStage::Fragment);
    CHECK(shaderStageFromPath("cull.comp") == ShaderStage::Compute);
    CHECK_FALSE(shaderStageFromPath("include/bindings.glsl").has_value());
    CHECK(nameOf(ShaderStage::Compute) == "compute");
}

TEST_CASE("Compiler logs from every common driver are rewritten to file:line") {
    const std::vector<std::string> files{"main.frag", "include/lighting.glsl"};
    // NVIDIA
    CHECK(annotateShaderLog("1(12) : error C1008: undefined variable \"foo\"\n", files) ==
          "include/lighting.glsl:12: error C1008: undefined variable \"foo\"\n");
    // glslang and AMD
    CHECK(annotateShaderLog("ERROR: 0:7: 'x' : undeclared identifier\n", files) ==
          "main.frag:7: error: 'x' : undeclared identifier\n");
    CHECK(annotateShaderLog("ERROR: 1:3: error(#143) Undeclared identifier: x\n", files) ==
          "include/lighting.glsl:3: error(#143) Undeclared identifier: x\n");
    CHECK(annotateShaderLog("WARNING: 0:2: 'y' : unused\n", files) == "main.frag:2: warning: 'y' : unused\n");
    // Mesa, with a column
    CHECK(annotateShaderLog("0:5(14): error: `foo' undeclared\n", files) ==
          "main.frag:5: error: `foo' undeclared\n");
    // Unknown source numbers and lines without positions
    CHECK(
        annotateShaderLog(
            "4(1) : error C0000: syntax error\nERROR: 1 compilation errors.  No code generated.\n", files) ==
        "<source 4>:1: error C0000: syntax error\nERROR: 1 compilation errors.  No code generated.\n");
    CHECK(annotateShaderLog("", files).empty());
    CHECK(annotateShaderLog("0(3) : error X", {}) == "<source 0>:3: error X\n");
}
