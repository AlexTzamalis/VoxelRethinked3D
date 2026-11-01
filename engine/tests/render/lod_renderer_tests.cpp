#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/lod_renderer.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <vector>

using namespace vxe;
using render::LodGeometry;
using render::LodRenderer;
using UploadResult = render::LodRenderer::UploadResult;

namespace {

std::unique_ptr<LodRenderer> makeLodRenderer(rhi::Device& device, config::SettingsRegistry& settings,
                                             stats::StatRegistry* stats = nullptr) {
    auto renderer = LodRenderer::create({.device = &device,
                                         .settings = &settings,
                                         .stats = stats,
                                         .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR),
                                         .arenaBytes = usize{4} << 20U,
                                         .maxTiles = 16,
                                         .stagingBytesPerFrame = usize{2} << 20U});
    if (!renderer) {
        MESSAGE(renderer.error().message());
    }
    REQUIRE(renderer.hasValue());
    return std::move(renderer.value());
}

// A tile of `quads` quads (their content does not matter here) at a tile
// position of level 1, around ground level.
struct FakeTile {
    std::vector<std::byte> bytes;
    LodGeometry geometry;

    FakeTile(u64 id, i64 tileX, i64 tileZ, u32 quads)
        : bytes(usize{quads} * render::kLodQuadBytes, std::byte{0}) {
        geometry = {.id = id,
                    .originX = tileX * 128,
                    .originZ = tileZ * 128,
                    .cellSize = 2,
                    .level = 1,
                    .baseY = 60,
                    .minY = 60,
                    .maxY = 70,
                    .quads = bytes};
    }
};

void submit(rhi::Device& device, LodRenderer& renderer) {
    rhi::CommandList commands;
    renderer.recordUploads(commands); // Draws need a render pass; the counts come from here.
    device.beginFrame();
    device.submit(commands);
    device.present();
    CHECK(device.stats().invalidCommands == 0);
}

} // namespace

TEST_CASE("LOD tiles upload, draw when in view and go away") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    auto renderer = makeLodRenderer(*device, settings, &stats);
    render::Camera camera;
    camera.position = DVec3(64.0, 100.0, 300.0); // Looking along -Z at tile (0, 0) and beyond.
    camera.pitch = -0.4F;

    const FakeTile front(1, 0, 0, 100);
    const FakeTile behind(2, 0, 5, 50);
    // Over 16,384 quads: two indirect draws.
    const FakeTile big(3, 0, -2, 20000);
    renderer->beginFrame(camera);
    REQUIRE(renderer->upload(front.geometry) == UploadResult::Uploaded);
    REQUIRE(renderer->upload(behind.geometry) == UploadResult::Uploaded);
    REQUIRE(renderer->upload(big.geometry) == UploadResult::Uploaded);
    submit(*device, *renderer);
    CHECK(renderer->tileCount() == 3);
    CHECK(renderer->quadCount() == 20150);
    CHECK(renderer->drawnTiles() == 2); // The tile behind the camera is culled.
    CHECK(renderer->drawCount() == 3);

    // Replacing keeps one tile; removing and clearing drop them.
    const FakeTile smaller(1, 0, 0, 10);
    renderer->beginFrame(camera);
    REQUIRE(renderer->upload(smaller.geometry) == UploadResult::Uploaded);
    CHECK(renderer->tileCount() == 3);
    CHECK(renderer->quadCount() == 20060);
    CHECK(renderer->remove(2));
    CHECK_FALSE(renderer->remove(2));
    submit(*device, *renderer);
    stats.endFrame(1.0 / 60.0);
    CHECK(stats.value(stats.find("Render.Lod.Tiles")) == 2.0);
    renderer->clear();
    renderer->beginFrame(camera);
    submit(*device, *renderer);
    CHECK(renderer->tileCount() == 0);
    CHECK(renderer->drawCount() == 0);
}

TEST_CASE("LOD uploads stop at the frame's budget and continue next frame") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeLodRenderer(*device, settings);
    REQUIRE(settings.set("Graphics.LOD.MaxUploadKBPerFrame", 64).hasValue());
    const FakeTile first(1, 0, 0, 4000); // 48 KB.
    const FakeTile second(2, 1, 0, 4000);
    renderer->beginFrame({});
    CHECK(renderer->upload(first.geometry) == UploadResult::Uploaded);
    CHECK(renderer->upload(second.geometry) == UploadResult::OverBudget);
    submit(*device, *renderer);
    renderer->beginFrame({});
    CHECK(renderer->upload(second.geometry) == UploadResult::Uploaded);
    // A tile bigger than the budget still goes when it is the first one.
    const FakeTile huge(3, 2, 0, 20000);
    submit(*device, *renderer);
    renderer->beginFrame({});
    CHECK(renderer->upload(huge.geometry) == UploadResult::Uploaded);
    submit(*device, *renderer);
}

TEST_CASE("A far tile is not hidden by the coverage of columns a whole map width away") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeLodRenderer(*device, settings);
    // A tile whole map widths ahead of the camera, on the same texels as columns
    // beside it. Those columns hold real chunks, the tile's own do not.
    const i32 mapBlocks = render::kLodCoverageSize * kChunkSize;
    const FakeTile far(1, mapBlocks / 128, 0, 6);
    for (i32 z = 0; z < 8; ++z) {
        for (i32 x = 0; x < 8; ++x) {
            renderer->setCovered(x, z, true);
        }
    }
    render::Camera camera;
    camera.yaw = 1.5707963F; // Along +X.
    camera.position = DVec3(8.0, 80.0, 64.0);
    renderer->beginFrame(camera);
    REQUIRE(renderer->upload(far.geometry) == UploadResult::Uploaded);
    submit(*device, *renderer);
    CHECK(renderer->drawnTiles() == 1);
}

TEST_CASE("The LOD coverage map wraps chunk columns around") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeLodRenderer(*device, settings);
    renderer->setCovered(-1, -1, true);
    CHECK(renderer->isCovered(-1, -1));
    CHECK(renderer->isCovered(255, 255)); // The same texel.
    CHECK(renderer->isCovered(-257, 511));
    CHECK_FALSE(renderer->isCovered(0, 0));
    // Covered part of the way up is not hidden as a whole.
    renderer->setCoveredBelow(5, 5, 12);
    CHECK_FALSE(renderer->isCovered(5, 5));
    renderer->setCovered(255, -257, false);
    CHECK_FALSE(renderer->isCovered(-1, -1));
    renderer->setCovered(5, 6, true);
    renderer->clearCoverage();
    CHECK_FALSE(renderer->isCovered(5, 6));
}
