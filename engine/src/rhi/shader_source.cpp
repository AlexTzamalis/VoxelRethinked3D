#include "vxe/rhi/shader_source.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

namespace vxe::rhi {
namespace {

// Includes deeper than this are almost certainly a mistake (or a cycle that
// slipped through), and the limit keeps a broken file from exhausting the stack.
constexpr usize kMaxIncludeDepth = 32;

[[nodiscard]] std::string_view trimLeft(std::string_view text) noexcept {
    const usize start = text.find_first_not_of(" \t");
    return start == std::string_view::npos ? std::string_view() : text.substr(start);
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    text = trimLeft(text);
    const usize end = text.find_last_not_of(" \t\r");
    return end == std::string_view::npos ? std::string_view() : text.substr(0, end + 1);
}

// If `line` is a preprocessor directive named `name` (spaces allowed after
// the '#'), returns the rest of the line, trimmed. Otherwise nothing.
[[nodiscard]] std::optional<std::string_view> directive(std::string_view line,
                                                        std::string_view name) noexcept {
    line = trimLeft(line);
    if (!line.starts_with('#')) {
        return std::nullopt;
    }
    line = trimLeft(line.substr(1));
    if (!line.starts_with(name)) {
        return std::nullopt;
    }
    const std::string_view rest = line.substr(name.size());
    if (!rest.empty() && rest.front() != ' ' && rest.front() != '\t' && rest.front() != '"' &&
        rest.front() != '<' && rest.front() != '\r') {
        return std::nullopt; // "#includes" or "#pragma_x" are other words.
    }
    return trim(rest);
}

// The paths a shader may include: relative, inside the root, '/' separated.
[[nodiscard]] bool isSafeIncludePath(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }
    usize start = 0;
    while (start <= path.size()) {
        const usize end = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

class Preprocessor {
public:
    explicit Preprocessor(const ShaderFileReader& reader)
        : m_reader(&reader) {}

    // Appends `path` (and everything it includes) to the output.
    // `from` is the include site ("file:line") for error messages, or empty
    // for the shader itself. Recursive through processLine on purpose: each
    // level is one nested include, bounded by kMaxIncludeDepth.
    // NOLINTNEXTLINE(misc-no-recursion)
    Result<void> include(std::string_view path, std::string_view from) {
        if (m_stack.size() >= kMaxIncludeDepth) {
            return Error(ErrorCode::InvalidArgument,
                         std::format("{}: includes nest deeper than {} files", from, kMaxIncludeDepth));
        }
        if (std::ranges::find(m_stack, path) != m_stack.end()) {
            std::string cycle;
            for (const std::string& file : m_stack) {
                cycle += file + " -> ";
            }
            return Error(ErrorCode::InvalidArgument,
                         std::format("{}: include cycle {}{}", from, cycle, path));
        }
        if (std::ranges::find(m_onceFiles, path) != m_onceFiles.end()) {
            return {};
        }
        auto text = (*m_reader)(path);
        if (!text) {
            if (from.empty()) {
                return Error(text.error().code(),
                             std::format("{}: cannot read: {}", path, text.error().message()));
            }
            return Error(text.error().code(),
                         std::format("{}: cannot include '{}': {}", from, path, text.error().message()));
        }

        const auto index = static_cast<u32>(fileIndex(path));
        m_stack.emplace_back(path);
        m_result.source += std::format("#line 1 {}\n", index);

        const std::string_view body = text.value();
        u32 lineNumber = 0;
        usize start = 0;
        while (start < body.size()) {
            const usize end = std::min(body.find('\n', start), body.size());
            const std::string_view line = body.substr(start, end - start);
            start = end + 1;
            ++lineNumber;
            VXE_TRY(processLine(line, path, index, lineNumber));
        }
        m_stack.pop_back();
        return {};
    }

    PreprocessedShader take() { return std::move(m_result); }
    std::string& source() { return m_result.source; }
    std::vector<std::string>& files() { return m_result.files; }

private:
    // NOLINTNEXTLINE(misc-no-recursion): see include().
    Result<void> processLine(std::string_view line, std::string_view path, u32 index, u32 lineNumber) {
        const std::string site = std::format("{}:{}", path, lineNumber);
        if (const auto target = directive(line, "include")) {
            if (target->size() < 2 || target->front() != '"' || target->back() != '"') {
                return Error(
                    ErrorCode::ParseError,
                    std::format("{}: write includes as #include \"path/from/shader/root.glsl\"", site));
            }
            const std::string_view includePath = target->substr(1, target->size() - 2);
            if (!isSafeIncludePath(includePath)) {
                return Error(ErrorCode::InvalidArgument,
                             std::format("{}: include path '{}' must be relative to the shader root, with "
                                         "'/' separators and no '..'",
                                         site, includePath));
            }
            VXE_TRY(include(includePath, site));
            // Back in this file: the next line keeps its real number.
            m_result.source += std::format("#line {} {}\n", lineNumber + 1, index);
            return {};
        }
        if (const auto pragma = directive(line, "pragma"); pragma && *pragma == "once") {
            m_onceFiles.emplace_back(path);
            m_result.source += '\n'; // Keeps the line count, so #line stays right.
            return {};
        }
        if (directive(line, "version")) {
            return Error(ErrorCode::ParseError,
                         std::format("{}: remove #version; the engine writes it for the device", site));
        }
        m_result.source += line;
        m_result.source += '\n';
        return {};
    }

    usize fileIndex(std::string_view path) {
        const auto found = std::ranges::find(m_result.files, path);
        if (found != m_result.files.end()) {
            return static_cast<usize>(found - m_result.files.begin());
        }
        m_result.files.emplace_back(path);
        return m_result.files.size() - 1;
    }

    const ShaderFileReader* m_reader; // Outlives the preprocessor (a local in preprocessShader).
    PreprocessedShader m_result;
    std::vector<std::string> m_stack;     // Files being included right now, outermost first.
    std::vector<std::string> m_onceFiles; // Files that said #pragma once.
};

[[nodiscard]] std::string_view stageDefine(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return "VXE_STAGE_VERTEX";
    case ShaderStage::Fragment:
        return "VXE_STAGE_FRAGMENT";
    case ShaderStage::Compute:
        return "VXE_STAGE_COMPUTE";
    }
    return "VXE_STAGE_UNKNOWN";
}

[[nodiscard]] std::optional<u32> parseNumber(std::string_view text) noexcept {
    u32 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// One log position: source string number, line, and the message after it.
struct LogPosition {
    u32 source = 0;
    u32 line = 0;
    std::string_view rest;
    std::string_view severity; // "error: " or "warning: " when the format puts it in front.
};

// Recognizes the position formats of the common GLSL compilers.
[[nodiscard]] std::optional<LogPosition> parseLogPosition(std::string_view line) noexcept {
    LogPosition result;
    if (line.starts_with("ERROR: ")) {
        result.severity = "error: ";
        line.remove_prefix(7);
    } else if (line.starts_with("WARNING: ")) {
        result.severity = "warning: ";
        line.remove_prefix(9);
    }
    const usize firstEnd = line.find_first_not_of("0123456789");
    if (firstEnd == 0 || firstEnd == std::string_view::npos) {
        return std::nullopt;
    }
    const auto source = parseNumber(line.substr(0, firstEnd));
    const char separator = line[firstEnd];
    if (!source || (separator != ':' && separator != '(')) {
        return std::nullopt;
    }
    const std::string_view afterSeparator = line.substr(firstEnd + 1);
    const usize numberEnd = afterSeparator.find_first_not_of("0123456789");
    if (numberEnd == 0 || numberEnd == std::string_view::npos) {
        return std::nullopt;
    }
    const auto lineNumber = parseNumber(afterSeparator.substr(0, numberEnd));
    if (!lineNumber) {
        return std::nullopt;
    }
    std::string_view rest = afterSeparator.substr(numberEnd);
    if (separator == '(') {
        // NVIDIA: "0(12) : error ..."
        if (!rest.starts_with(')')) {
            return std::nullopt;
        }
        rest.remove_prefix(1);
    } else if (rest.starts_with('(')) {
        // Mesa: "0:12(5): error ..." carries a column we do not need.
        const usize close = rest.find(')');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        rest.remove_prefix(close + 1);
    }
    rest = trimLeft(rest);
    if (!rest.starts_with(':')) {
        return std::nullopt;
    }
    rest = trimLeft(rest.substr(1));
    if (rest.starts_with("error") || rest.starts_with("warning")) {
        result.severity = {}; // Already says what it is.
    }
    result.source = *source;
    result.line = *lineNumber;
    result.rest = rest;
    return result;
}

} // namespace

Result<PreprocessedShader> preprocessShader(std::string_view path, const ShaderPreprocessOptions& options,
                                            const ShaderFileReader& reader) {
    Preprocessor preprocessor(reader);
    std::string& source = preprocessor.source();
    source += std::format("#version {} core\n", options.glslVersion);
    // Only OpenGL style binding exists so far; the Null device mirrors it.
    // A Vulkan backend would add a case that defines VXE_VULKAN instead.
    switch (options.backend) {
    case Backend::Null:
    case Backend::OpenGL:
        source += "#define VXE_OPENGL 1\n";
        break;
    }
    source += std::format("#define {} 1\n", stageDefine(options.stage));
    for (const ShaderDefine& define : options.defines) {
        source += std::format("#define {} {}\n", define.name, define.value);
    }
    VXE_TRY(preprocessor.include(path, {}));
    return preprocessor.take();
}

ShaderFileReader folderShaderReader(std::filesystem::path root) {
    return [root = std::move(root)](std::string_view path) -> Result<std::string> {
        const std::filesystem::path full = root / std::filesystem::path(path);
        const std::ifstream file(full, std::ios::binary);
        if (!file) {
            return Error(ErrorCode::NotFound, std::format("'{}' does not exist", full.generic_string()));
        }
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    };
}

Result<ShaderHandle> createShaderFromFile(Device& device, std::string_view path,
                                          const ShaderFileReader& reader,
                                          std::span<const ShaderDefine> defines) {
    const auto stage = shaderStageFromPath(path);
    if (!stage) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("'{}' is not a shader stage file (.vert, .frag or .comp)", path));
    }
    const ShaderPreprocessOptions options{.stage = *stage,
                                          .backend = device.capabilities().backend,
                                          .defines =
                                              std::vector<ShaderDefine>(defines.begin(), defines.end())};
    const auto source = preprocessShader(path, options, reader);
    if (!source) {
        return source.error();
    }
    return device.createShader({.stage = *stage,
                                .source = source.value().source,
                                .fileNames = source.value().files,
                                .debugName = path});
}

std::optional<ShaderStage> shaderStageFromPath(std::string_view path) noexcept {
    if (path.ends_with(".vert")) {
        return ShaderStage::Vertex;
    }
    if (path.ends_with(".frag")) {
        return ShaderStage::Fragment;
    }
    if (path.ends_with(".comp")) {
        return ShaderStage::Compute;
    }
    return std::nullopt;
}

std::string_view nameOf(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vertex";
    case ShaderStage::Fragment:
        return "fragment";
    case ShaderStage::Compute:
        return "compute";
    }
    return "unknown";
}

std::string annotateShaderLog(std::string_view log, std::span<const std::string> files) {
    std::string result;
    usize start = 0;
    while (start < log.size()) {
        const usize end = std::min(log.find('\n', start), log.size());
        const std::string_view line = trim(log.substr(start, end - start));
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        if (const auto position = parseLogPosition(line)) {
            const std::string file = position->source < files.size()
                                         ? files[position->source]
                                         : std::format("<source {}>", position->source);
            result += std::format("{}:{}: {}{}\n", file, position->line, position->severity, position->rest);
        } else {
            result += line;
            result += '\n';
        }
    }
    return result;
}

} // namespace vxe::rhi
