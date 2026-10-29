#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/time.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/render/lod_renderer.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/voxel/lod_render_sink.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <thread>

using namespace vxe;
using voxel::LodSpan;
using voxel::LodTileKey;

namespace {

class FlatSampler final : public voxel::LodTileSampler {
public:
    bool sampleTile(const LodTileKey& /*key*/, const voxel::LodSampleOptions& /*options*/,
                    voxel::LodTileBuilder& builder, const std::atomic<bool>& /*cancelled*/) override {
        const LodSpan ground{.bottomY = 0, .topY = 64, .topMaterial = 1, .sideMaterial = 2};
        for (u32 i = 0; i < voxel::kLodCellCount; ++i) {
            builder.addCell(std::span(&ground, 1));
        }
        return true;
    }
    [[nodiscard]] u64 worldHash() const override { return 7; }
};

// Takes every chunk mesh.
class AcceptingSink final : public voxel::ChunkMeshSink {
public:
    [[nodiscard]] Result upload(const voxel::ChunkMesh& /*mesh*/) override { return Result::Accepted; }
    void remove(ChunkPos /*position*/) override {}
};

std::unique_ptr<render::LodRenderer> makeRenderer(rhi::Device& device, config::SettingsRegistry& settings) {
    auto renderer = render::LodRenderer::create({.device = &device,
                                                 .settings = &settings,
                                                 .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR),
                                                 .arenaBytes = usize{32} << 20U});
    REQUIRE(renderer.hasValue());
    return std::move(renderer.value());
}

voxel::ChunkMesh meshAt(ChunkPos position) {
    voxel::ChunkMesh mesh;
    mesh.position = position;
    return mesh;
}

} // namespace

TEST_CASE("The LOD render sink meshes and uploads every tile the manager shows") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    FlatSampler sampler;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 4, .ioThreadCount = 1}};
    auto renderer = makeRenderer(*device, settings);
    voxel::LodRenderSink sink(*renderer, jobs);
    auto manager = std::make_unique<voxel::LodManager>(
        voxel::LodManagerDesc{.jobs = &jobs, .settings = &settings, .sampler = &sampler, .sink = &sink});
    render::Camera camera;
    camera.position = DVec3(0.0, 100.0, 0.0);
    const Stopwatch clock;
    do {
        renderer->beginFrame(camera);
        manager->update(camera.position);
        sink.update();
        rhi::CommandList commands;
        renderer->recordUploads(commands);
        device->beginFrame();
        device->submit(commands);
        device->present();
        std::this_thread::yield();
    } while ((!manager->isSettled() || !sink.isIdle()) && clock.elapsedMilliseconds() < 20000.0);
    CHECK(renderer->tileCount() == 304);
    // A flat tile is one top and four skirts.
    CHECK(renderer->quadCount() == 304 * 5);
    CHECK(renderer->isResident(voxel::lodTileId({.level = 1, .x = 0, .z = 0})));
    manager.reset();
}

TEST_CASE("A chunk column is covered once every chunk from its surfaces up is meshed") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    jobs::JobSystem jobs{jobs::JobSystemConfig{.workerCount = 1, .ioThreadCount = 1}};
    auto renderer = makeRenderer(*device, settings);
    voxel::VoxelWorld world;
    voxel::LodRenderSink lod(*renderer, jobs, &world);
    AcceptingSink chunks;
    voxel::CoveringChunkSink sink(chunks, lod);

    // Column (2, -1): ground at y 40 in one corner (chunk 2) and a hill up to
    // y 70 in another (chunk 4), with chunk 5 above.
    voxel::ChunkStorage low;
    low.set({.x = 0, .y = 8, .z = 0}, 1);
    voxel::ChunkStorage high;
    high.set({.x = 5, .y = 6, .z = 5}, 1);
    world.insertChunk({.x = 2, .y = 2, .z = -1}, low);
    world.insertChunk({.x = 2, .y = 3, .z = -1});
    world.insertChunk({.x = 2, .y = 4, .z = -1}, high);
    world.insertChunk({.x = 2, .y = 5, .z = -1});

    const auto meshed = [&](i32 y) {
        REQUIRE(sink.upload(meshAt({.x = 2, .y = y, .z = -1})) == voxel::ChunkMeshSink::Result::Accepted);
        lod.update();
    };
    meshed(4);
    meshed(5);
    CHECK_FALSE(renderer->isCovered(2, -1)); // The low ground (chunks 2 and 3) is not drawn yet.
    meshed(2);
    CHECK_FALSE(renderer->isCovered(2, -1));
    meshed(3);
    CHECK(renderer->isCovered(2, -1));
    sink.remove({.x = 2, .y = 2, .z = -1});
    lod.update();
    CHECK_FALSE(renderer->isCovered(2, -1));
    CHECK_FALSE(renderer->isCovered(3, -1));
}
