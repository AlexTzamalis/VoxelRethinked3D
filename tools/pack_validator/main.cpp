// pack_validator (G2.4, Section 15.4): checks a texture pack folder.
//
//   vxe_pack_validator <pack folder> [--reference <pack folder>]
//
// Prints every problem render::validatePack finds, errors first. The
// reference pack (default: the "default" folder next to the checked pack)
// gives the texture keys the game uses, to report missing and unused
// textures. Exit code 0 without errors (warnings are fine), 1 with errors,
// 2 for wrong arguments.

#include <vxe/render/texture_pack.hpp>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void print(const std::string& line) {
    std::fputs((line + "\n").c_str(), stdout);
}

// The texture keys of a pack folder: the names of its blocks/*.png files.
std::vector<std::string> keysOf(const fs::path& pack) {
    std::vector<std::string> keys;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(pack / "blocks", error)) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".png")) {
            keys.push_back(name.substr(0, name.size() - 4));
        }
    }
    std::ranges::sort(keys);
    return keys;
}

int run(int argc, char** argv) {
    std::vector<std::string> positional;
    std::optional<fs::path> reference;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--reference" && i + 1 < argc) {
            reference = argv[++i];
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 1) {
        print("usage: vxe_pack_validator <pack folder> [--reference <pack folder>]");
        return 2;
    }
    const fs::path pack = positional[0];
    std::error_code error;
    const fs::path sibling = pack.parent_path() / "default";
    if (!reference && fs::is_directory(sibling, error) &&
        fs::weakly_canonical(pack, error) != fs::weakly_canonical(sibling, error)) {
        reference = sibling;
    }
    const std::vector<std::string> keys = reference ? keysOf(*reference) : std::vector<std::string>{};
    std::vector<vxe::render::PackIssue> issues = vxe::render::validatePack(pack, keys);
    std::ranges::stable_sort(issues, [](const auto& a, const auto& b) { return a.error && !b.error; });
    std::size_t errors = 0;
    for (const vxe::render::PackIssue& issue : issues) {
        errors += issue.error ? 1 : 0;
        print(std::format("{}: {}{}{}", issue.error ? "error" : "warning", issue.file,
                          issue.file.empty() ? "" : ": ", issue.message));
    }
    print(std::format("{}: {} errors, {} warnings{}", pack.string(), errors, issues.size() - errors,
                      reference ? std::format(" (keys from {})", reference->string()) : std::string()));
    return errors == 0 ? 0 : 1;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): reporting a failure may throw, which ends the tool as well.
int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fputs((std::string("pack_validator: ") + error.what() + "\n").c_str(), stderr);
    }
    return 2;
}
