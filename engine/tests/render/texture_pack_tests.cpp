#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/io/file_io.hpp>
#include <vxe/io/file_watcher.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/render/texture_pack.hpp>
#include <vxe/render/texture_pack_manager.hpp>
#include <vxe/rhi_null/null_device.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <doctest/doctest.h>
#include <stb_image_write.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace vxe;
using render::BlockTextureData;
using render::PackSource;

namespace {

constexpr u32 kRed = 0xFF0000FFU;
constexpr u32 kGreen = 0xFF00FF00U;
constexpr u32 kBlue = 0xFFFF0000U;

std::filesystem::path freshFolder(std::string_view name) {
    const auto folder = std::filesystem::temp_directory_path() / "vxe_tests" / "packs" / std::string(name);
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder);
    return folder;
}

// A width x height PNG, pixel (x, y) from the top left given by `color`.
template <typename Color>
std::vector<std::byte> png(int width, int height, Color color) {
    std::vector<u32> pixels(static_cast<usize>(width) * static_cast<usize>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[(static_cast<usize>(y) * static_cast<usize>(width)) + static_cast<usize>(x)] = color(x, y);
        }
    }
    std::vector<std::byte> out;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* bytes = static_cast<std::vector<std::byte>*>(context);
            const auto* begin = static_cast<const std::byte*>(data);
            bytes->insert(bytes->end(), begin, begin + size);
        },
        &out, width, height, 4, pixels.data(), width * 4);
    return out;
}

std::vector<std::byte> solid(int size, u32 color) {
    return png(size, size, [color](int, int) { return color; });
}

void writePack(const std::filesystem::path& folder, u32 resolution,
               const std::vector<std::pair<std::string, std::vector<std::byte>>>& files) {
    const std::string manifest =
        std::format("name = \"{}\"\nformat = 1\nresolution = {}\n", folder.filename().string(), resolution);
    REQUIRE(io::writeFile(folder / "pack.toml", std::as_bytes(std::span(manifest))).hasValue());
    for (const auto& [path, bytes] : files) {
        REQUIRE(io::writeFile(folder / path, bytes).hasValue());
    }
}

std::unique_ptr<PackSource> open(const std::filesystem::path& folder) {
    auto pack = render::openPackFolder(folder);
    REQUIRE(pack.hasValue());
    return std::move(pack.value());
}

u32 texel(const BlockTextureData& data, u32 layer, u32 x, u32 y, u32 mip = 0) {
    const u32 side = data.resolution >> mip;
    return data.pixels[mip][(usize{layer} * side * side) + (usize{y} * side) + x];
}

} // namespace

TEST_CASE("Pack manifests are checked") {
    auto ok = render::parsePackManifest("name = \"A\"\nformat = 1\nresolution = 32\n[options]\n"
                                        "mipmap_alpha_preserve = false\n",
                                        "a");
    REQUIRE(ok.hasValue());
    CHECK(ok.value().id == "a");
    CHECK(ok.value().resolution == 32);
    CHECK_FALSE(ok.value().mipmapAlphaPreserve);
    CHECK(render::parsePackManifest("name = \"A\"\nformat = 2\nresolution = 16\n", "a").error().code() ==
          ErrorCode::VersionMismatch);
    CHECK(render::parsePackManifest("name = \"A\"\nformat = 1\nresolution = 20\n", "a").error().code() ==
          ErrorCode::InvalidArgument);
    CHECK(render::parsePackManifest("format = 1\nresolution = 16\n", "a").error().code() ==
          ErrorCode::ParseError);
    CHECK(render::parsePackManifest("name = [", "a").error().code() == ErrorCode::ParseError);
}

TEST_CASE("A broken image in a higher pack falls back to the next pack") {
    const auto root = freshFolder("fallback");
    writePack(root / "low", 16, {{"blocks/dirt.png", solid(16, kGreen)}});
    writePack(root / "high", 16, {{"blocks/dirt.png", std::vector<std::byte>(12, std::byte{7})}});
    const auto low = open(root / "low");
    const auto high = open(root / "high");
    const std::vector<const PackSource*> stack = {high.get(), low.get()};
    const std::vector<std::string> keys = {"dirt"};
    const BlockTextureData data = render::buildBlockTextures(stack, keys);
    CHECK(texel(data, 0, 2, 2) == kGreen);
    CHECK(data.sources[0] == "low");
    REQUIRE(data.warnings.size() == 1);
    CHECK(data.warnings[0].find("pack 'high'") != std::string::npos);
}

TEST_CASE("Higher packs override lower ones by key; missing keys get the checker") {
    const auto root = freshFolder("stack");
    writePack(root / "base", 16,
              {{"blocks/stone.png", solid(16, kRed)}, {"blocks/dirt.png", solid(16, kGreen)}});
    writePack(root / "fancy", 16, {{"blocks/stone.png", solid(16, kBlue)}});
    std::vector<std::string> problems;
    const auto packs = render::discoverPacks(root, problems);
    REQUIRE(packs.size() == 2);
    CHECK(packs[0]->manifest().id == "base");
    CHECK(problems.empty());

    const std::vector<const PackSource*> stack = {packs[1].get(), packs[0].get()};
    const std::vector<std::string> keys = {"stone", "dirt", "nothing"};
    const BlockTextureData data = render::buildBlockTextures(stack, keys);
    CHECK(data.layers == 3);
    CHECK(data.resolution == 16);
    CHECK(data.mips == 5);
    CHECK(texel(data, 0, 3, 3) == kBlue); // From "fancy", over "base".
    CHECK(texel(data, 1, 3, 3) == kGreen);
    CHECK(data.sources == std::vector<std::string>{"fancy", "base", ""});
    CHECK((texel(data, 2, 0, 0) == 0xFFFF00FFU || texel(data, 2, 0, 0) == 0xFF000000U)); // The checker.
    REQUIRE(data.warnings.size() == 1);
    CHECK(data.warnings[0].find("blocks/nothing.png") != std::string::npos);
    CHECK(data.averageColors[0] == kBlue);
}

TEST_CASE("Mixed resolutions scale to the largest, flipped bottom row first") {
    const auto root = freshFolder("sizes");
    // The top half red, the bottom half green, at 16 pixels; another pack at 32.
    writePack(root / "small", 16,
              {{"blocks/half.png", png(16, 16, [](int, int y) { return y < 8 ? kRed : kGreen; })}});
    writePack(root / "big", 32, {{"blocks/other.png", solid(32, kBlue)}});
    const auto small = open(root / "small");
    const auto big = open(root / "big");
    const std::vector<const PackSource*> stack = {big.get(), small.get()};
    const std::vector<std::string> keys = {"half", "other"};
    BlockTextureData data = render::buildBlockTextures(stack, keys);
    CHECK(data.resolution == 32);
    CHECK(data.mips == 6);
    CHECK(texel(data, 0, 5, 0) == kGreen); // Row 0 is the bottom of the image.
    CHECK(texel(data, 0, 5, 31) == kRed);
    CHECK(texel(data, 1, 0, 0) == kBlue);
    // The resolution limit wins over the packs.
    data = render::buildBlockTextures(stack, keys, {.maxResolution = 16});
    CHECK(data.resolution == 16);
}

TEST_CASE("Animated textures put their frames after the keys") {
    const auto root = freshFolder("anim");
    // Three square frames stacked: red, green, blue, 4 ticks each.
    const std::string anim = "frametime = 4\n";
    writePack(root / "a", 16,
              {{"blocks/water.png", png(16, 48,
                                        [](int, int y) {
                                            if (y < 16) {
                                                return kRed;
                                            }
                                            return y < 32 ? kGreen : kBlue;
                                        })},
               {"blocks/water.anim.toml", {}},
               {"blocks/stone.png", solid(16, kRed)}});
    REQUIRE(
        io::writeFile(root / "a" / "blocks" / "water.anim.toml", std::as_bytes(std::span(anim))).hasValue());
    const auto pack = open(root / "a");
    const std::vector<const PackSource*> stack = {pack.get()};
    const std::vector<std::string> keys = {"water", "stone"};
    const BlockTextureData data = render::buildBlockTextures(stack, keys);
    CHECK(data.layers == 4);
    CHECK(texel(data, 0, 0, 0) == kRed);
    CHECK(texel(data, 2, 0, 0) == kGreen); // Frames 2 and 3 follow the 2 keys.
    CHECK(texel(data, 3, 0, 0) == kBlue);
    CHECK(data.animations[0] == (3U | (4U << 8U) | (2U << 16U)));
    CHECK(data.animations[1] == 0);
}

TEST_CASE("Cut out textures keep their coverage in every mip") {
    const auto root = freshFolder("leaves");
    // Leaves: a third of the pixels clear, scattered.
    writePack(root / "a", 16, {{"blocks/leaves.png", png(16, 16, [](int x, int y) {
                                    return ((x * 7) + (y * 3)) % 3 == 0 ? 0U : kGreen;
                                })}});
    const auto pack = open(root / "a");
    const std::vector<const PackSource*> stack = {pack.get()};
    const std::vector<std::string> keys = {"leaves"};
    const BlockTextureData data = render::buildBlockTextures(stack, keys);
    for (u32 mip = 1; mip < data.mips; ++mip) {
        const u32 side = data.resolution >> mip;
        usize visible = 0;
        for (u32 i = 0; i < side * side; ++i) {
            visible += (data.pixels[mip][i] >> 24U) >= 128 ? 1U : 0U;
        }
        const f64 share = static_cast<f64>(visible) / static_cast<f64>(side * side);
        // Two thirds visible, within what a small mip can show.
        CHECK(share >= (mip + 1 == data.mips ? 1.0 : 0.5));
        CHECK(share <= 1.0);
    }
    // Colors are averaged by alpha, so the clear (black) pixels do not darken them.
    CHECK(((data.averageColors[0] >> 8U) & 0xFFU) == 255);
}

TEST_CASE("Broken images and packs are warnings, never failures") {
    const auto root = freshFolder("broken");
    std::vector<std::byte> junk(64, std::byte{0x42});
    writePack(root / "a", 16,
              {{"blocks/junk.png", junk}, {"blocks/tall.png", png(16, 20, [](int, int) { return kRed; })}});
    REQUIRE(io::writeFile(root / "nomanifest" / "blocks" / "x.png", solid(16, kRed)).hasValue());
    std::vector<std::string> problems;
    const auto packs = render::discoverPacks(root, problems);
    CHECK(packs.size() == 1);
    CHECK(problems.size() == 1);
    const std::vector<const PackSource*> stack = {packs[0].get()};
    const std::vector<std::string> keys = {"junk", "tall"};
    const BlockTextureData data = render::buildBlockTextures(stack, keys);
    CHECK(data.layers == 2);
    CHECK(data.warnings.size() == 2);
}

TEST_CASE("Built in packs read from memory") {
    static constexpr std::string_view kManifest = "name = \"Built in\"\nformat = 1\nresolution = 16\n";
    const auto bytes = solid(16, kGreen);
    const std::vector<unsigned char> image(
        reinterpret_cast<const unsigned char*>(bytes.data()), // NOLINT
        reinterpret_cast<const unsigned char*>(bytes.data() + bytes.size()));
    const std::vector<render::EmbeddedFile> files = {
        {.path = "pack.toml",
         .bytes =
             std::span(reinterpret_cast<const unsigned char*>(kManifest.data()), kManifest.size())}, // NOLINT
        {.path = "blocks/grass.png", .bytes = image}};
    auto pack = render::openEmbeddedPack("default", files);
    REQUIRE(pack.hasValue());
    CHECK(pack.value()->manifest().name == "Built in");
    const std::vector<const PackSource*> stack = {pack.value().get()};
    const std::vector<std::string> keys = {"grass"};
    CHECK(texel(render::buildBlockTextures(stack, keys), 0, 1, 1) == kGreen);
}

TEST_CASE("The pack manager loads on a job and swaps packs live") {
    const auto root = freshFolder("manager");
    writePack(root / "default", 16,
              {{"blocks/stone.png", solid(16, kRed)}, {"blocks/dirt.png", solid(16, kGreen)}});
    writePack(root / "fancy", 16, {{"blocks/stone.png", solid(16, kBlue)}});
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 2, .ioThreadCount = 1}};
    auto created = render::TexturePackManager::create(
        {.device = device.get(), .settings = &settings, .jobs = &jobs, .packsFolder = root});
    REQUIRE(created.hasValue());
    render::TexturePackManager& manager = *created.value();
    CHECK(manager.textures() == nullptr);
    manager.setKeys({"stone", "dirt"});
    const auto waitForSwap = [&] {
        const Stopwatch clock;
        while (!manager.update() && clock.elapsedMilliseconds() < 10000.0) {
            settings.dispatchChanges();
            std::this_thread::yield();
        }
    };
    waitForSwap();
    REQUIRE(manager.textures() != nullptr);
    CHECK(manager.textures()->averageColors[0] == kRed);
    CHECK(manager.activeStack().size() == 1);
    CHECK(manager.availablePacks().size() == 2);

    // Enabling "fancy" loads again, live.
    REQUIRE(settings.set("Graphics.Textures.Packs", config::SettingValue{std::string("fancy")}).hasValue());
    settings.dispatchChanges();
    CHECK(manager.isLoading());
    waitForSwap();
    CHECK(manager.textures()->averageColors[0] == kBlue);
    const auto stack = manager.activeStack();
    CHECK(std::vector<std::string>(stack.begin(), stack.end()) ==
          std::vector<std::string>{"fancy", "default"});

    // An unknown pack is a warning; the rest still loads.
    REQUIRE(settings.set("Graphics.Textures.Packs", config::SettingValue{std::string("missing, fancy")})
                .hasValue());
    settings.dispatchChanges();
    waitForSwap();
    CHECK(manager.warnings().size() == 1);
    CHECK(manager.textures()->averageColors[0] == kBlue);
    CHECK_FALSE(device->liveResources().empty());
}

TEST_CASE("Saving a texture reloads the packs live (hot reload), and another folder can be chosen") {
    const auto root = freshFolder("hot");
    writePack(root / "default", 16, {{"blocks/stone.png", solid(16, kRed)}});
    const auto other = freshFolder("hot_other");
    writePack(other / "default", 16, {{"blocks/stone.png", solid(16, kGreen)}});
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 2, .ioThreadCount = 1}};
    io::FileWatcher watcher(std::chrono::milliseconds(50));
    auto created = render::TexturePackManager::create({.device = device.get(),
                                                       .settings = &settings,
                                                       .jobs = &jobs,
                                                       .packsFolder = root,
                                                       .watcher = &watcher});
    REQUIRE(created.hasValue());
    render::TexturePackManager& manager = *created.value();
    REQUIRE(settings.set("Graphics.Textures.HotReload", config::SettingValue{true}).hasValue());
    settings.dispatchChanges();
    manager.setKeys({"stone"});
    const auto waitFor = [&](u32 color) {
        const Stopwatch clock;
        while (clock.elapsedMilliseconds() < 10000.0) {
            watcher.poll();
            settings.dispatchChanges();
            manager.update();
            if (manager.textures() != nullptr && manager.textures()->averageColors[0] == color) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    REQUIRE(waitFor(kRed));

    // An image editor saves the texture.
    REQUIRE(io::writeFile(root / "default" / "blocks" / "stone.png", solid(16, kBlue)).hasValue());
    CHECK(waitFor(kBlue));
    CHECK(manager.hotReloads() >= 1);

    // Another packs folder, chosen by setting.
    REQUIRE(settings.set("Graphics.Textures.Folder", config::SettingValue{other.string()}).hasValue());
    settings.dispatchChanges();
    CHECK(waitFor(kGreen));

    // With hot reload off, saving changes nothing.
    REQUIRE(settings.set("Graphics.Textures.HotReload", config::SettingValue{false}).hasValue());
    settings.dispatchChanges();
    REQUIRE(io::writeFile(other / "default" / "blocks" / "stone.png", solid(16, kRed)).hasValue());
    const Stopwatch quiet;
    while (quiet.elapsedMilliseconds() < 500.0) {
        watcher.poll();
        manager.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(manager.isLoading());
    CHECK(manager.textures()->averageColors[0] == kGreen);
}

TEST_CASE("The pack validator reports errors and warnings with their files") {
    const auto root = freshFolder("validate");
    const std::string badAnim = "frametime = [";
    const std::string slowAnim = "frametime = 900\n";
    writePack(root / "a", 16,
              {{"blocks/stone.png", solid(16, kRed)},
               {"blocks/odd.png", solid(12, kRed)},
               {"blocks/tall.png", png(16, 20, [](int, int) { return kRed; })},
               {"blocks/big.png", solid(32, kRed)},
               {"blocks/strip.png", png(16, 32, [](int, int) { return kRed; })},
               {"blocks/junk.png", std::vector<std::byte>(10, std::byte{1})}});
    REQUIRE(io::writeFile(root / "a" / "blocks" / "ghost.anim.toml", std::as_bytes(std::span(slowAnim)))
                .hasValue());
    REQUIRE(io::writeFile(root / "a" / "blocks" / "stone.anim.toml", std::as_bytes(std::span(badAnim)))
                .hasValue());
    const std::vector<std::string> reference = {"stone", "dirt", "odd", "tall", "big", "strip", "junk"};
    const std::vector<render::PackIssue> issues = render::validatePack(root / "a", reference);
    const auto find = [&](std::string_view file, bool error) {
        return std::ranges::count_if(issues, [&](const render::PackIssue& issue) {
            return issue.file == file && issue.error == error;
        });
    };
    CHECK(find("blocks/odd.png", true) == 1);    // 12 wide: not a power of two.
    CHECK(find("blocks/tall.png", true) == 1);   // 16 x 20: not square frames.
    CHECK(find("blocks/junk.png", true) == 1);   // Not a PNG.
    CHECK(find("blocks/big.png", false) == 1);   // 32 in a 16 pack: scaled.
    CHECK(find("blocks/strip.png", false) == 1); // Frames without an .anim.toml.
    CHECK(find("blocks/stone.anim.toml", true) == 1);
    CHECK(find("blocks/ghost.anim.toml", false) == 2); // No ghost.png, frametime out of range.
    CHECK(find("blocks/dirt.png", false) == 1);        // Missing: falls back.
    CHECK(issues.size() == 10);

    const auto noManifest = freshFolder("validate_none");
    const auto none = render::validatePack(noManifest, reference);
    REQUIRE(none.size() == 1);
    CHECK(none[0].error);
    CHECK(none[0].file == "pack.toml");
}
