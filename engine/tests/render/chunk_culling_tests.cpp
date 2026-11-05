#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/jobs/job_system.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/chunk_culling.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/render/hi_z.hpp>
#include <vxe/render/renderer.hpp>
#include <vxe/rhi_gl/gl_device.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/voxel/mesher.hpp>

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <numbers>
#include <thread>
#include <vector>

using namespace vxe;
using render::CaveCuller;
using render::ChunkPass;
using render::ChunkRenderer;
using render::kChunkAllConnected;
using UploadResult = ChunkRenderer::UploadResult;

namespace {

// Joins only the listed face pairs.
u16 connecting(std::initializer_list<std::pair<u32, u32>> pairs) {
    u16 mask = 0;
    for (const auto& [a, b] : pairs) {
        mask = static_cast<u16>(mask | render::chunkConnectivityBit(a, b));
    }
    return mask;
}

constexpr u32 kPosX = 0;
constexpr u32 kNegX = 1;
constexpr u32 kPosZ = 4;
constexpr u32 kNegZ = 5;

} // namespace

TEST_CASE("The renderer's connectivity bits match the mesher's") {
    for (u32 a = 0; a < 6; ++a) {
        for (u32 b = 0; b < 6; ++b) {
            if (a != b) {
                CHECK(render::chunkConnectivityBit(a, b) ==
                      voxel::connectivityBit(static_cast<voxel::BlockFace>(a),
                                             static_cast<voxel::BlockFace>(b)));
            }
        }
    }
    CHECK(kChunkAllConnected == voxel::kAllConnected);
}

TEST_CASE("Cave culling sees everything in open space and nothing behind a closed shell") {
    CaveCuller caves;
    caves.update({});
    CHECK(caves.isVisible({.x = 5, .y = -3, .z = 40})); // Nothing known: all open.

    // A shell of solid chunks around the camera's chunk.
    for (i32 z = -1; z <= 1; ++z) {
        for (i32 y = -1; y <= 1; ++y) {
            for (i32 x = -1; x <= 1; ++x) {
                if (x != 0 || y != 0 || z != 0) {
                    caves.setChunk({.x = x, .y = y, .z = z}, 0);
                }
            }
        }
    }
    // Space beyond the shell, inside the searched box.
    caves.setChunk({.x = 3, .y = 0, .z = 0}, 0);
    caves.setChunk({.x = -3, .y = 0, .z = 0}, 0);
    CHECK(caves.update({}));
    CHECK_FALSE(caves.update({})); // Nothing changed: no new search.
    CHECK(caves.isVisible({}));
    CHECK(caves.isVisible({.x = 1, .y = 0, .z = 0}));       // Face neighbors are seen (their walls).
    CHECK_FALSE(caves.isVisible({.x = 1, .y = 1, .z = 0})); // Edge neighbors are not reached.
    CHECK_FALSE(caves.isVisible({.x = 2, .y = 0, .z = 0}));
    CHECK_FALSE(caves.isVisible({.x = 3, .y = 0, .z = 0}));
    CHECK(caves.isVisible({.x = 40, .y = 0, .z = 0})); // Outside the searched box: assumed visible.

    // Opening one wall lets the view out on that side only.
    caves.setChunk({.x = 1, .y = 0, .z = 0}, kChunkAllConnected);
    CHECK(caves.update({}));
    CHECK(caves.isVisible({.x = 2, .y = 0, .z = 0}));
    CHECK(caves.isVisible({.x = 3, .y = 0, .z = 0}));
    CHECK(caves.isVisible({.x = 2, .y = 1, .z = 0}));
    CHECK_FALSE(caves.isVisible({.x = -2, .y = 0, .z = 0}));
}

TEST_CASE("Cave culling follows tunnels but never sees back around a bend") {
    // Rock everywhere in a 9 x 3 x 9 block, with a tunnel that runs +X from
    // the camera, turns +Z, and then turns back -X.
    CaveCuller caves;
    for (i32 z = -1; z <= 7; ++z) {
        for (i32 y = -1; y <= 1; ++y) {
            for (i32 x = -1; x <= 7; ++x) {
                caves.setChunk({.x = x, .y = y, .z = z}, 0);
            }
        }
    }
    const u16 straightX = connecting({{kPosX, kNegX}});
    const u16 straightZ = connecting({{kPosZ, kNegZ}});
    caves.setChunk({}, kChunkAllConnected);
    for (i32 x = 1; x < 6; ++x) {
        caves.setChunk({.x = x, .y = 0, .z = 0}, straightX);
    }
    caves.setChunk({.x = 6, .y = 0, .z = 0}, connecting({{kNegX, kPosZ}}));
    for (i32 z = 1; z < 4; ++z) {
        caves.setChunk({.x = 6, .y = 0, .z = z}, straightZ);
    }
    caves.setChunk({.x = 6, .y = 0, .z = 4}, connecting({{kNegZ, kNegX}}));
    for (i32 x = 0; x < 6; ++x) {
        caves.setChunk({.x = x, .y = 0, .z = 4}, straightX);
    }
    caves.update({});
    CHECK(caves.isVisible({.x = 5, .y = 0, .z = 0}));
    CHECK(caves.isVisible({.x = 6, .y = 0, .z = 3})); // Around one bend.
    CHECK(caves.isVisible({.x = 6, .y = 0, .z = 4}));
    // Going -X after +X would mean looking back past the start: hidden.
    CHECK_FALSE(caves.isVisible({.x = 5, .y = 0, .z = 4}));
    CHECK_FALSE(caves.isVisible({.x = 0, .y = 0, .z = 4}));
    CHECK_FALSE(caves.isVisible({.x = 3, .y = 1, .z = 0})); // The rock around the tunnel.
    MESSAGE("Cave search: " << caves.reachedCells() << " cells reached in " << caves.lastSearchMilliseconds()
                            << " ms");
}

TEST_CASE("Cave culling on a job gives the same answers, and everything until they are in") {
    jobs::JobSystem jobs(jobs::JobSystemConfig{.workerCount = 2, .ioThreadCount = 1});
    CaveCuller onThread;
    CaveCuller onJob;
    onJob.setJobSystem(&jobs);
    // Rock with a tunnel along +X from the camera.
    for (CaveCuller* caves : {&onThread, &onJob}) {
        for (i32 z = -1; z <= 1; ++z) {
            for (i32 y = -1; y <= 1; ++y) {
                for (i32 x = -1; x <= 6; ++x) {
                    caves->setChunk({.x = x, .y = y, .z = z}, 0);
                }
            }
        }
        caves->setChunk({}, kChunkAllConnected);
        for (i32 x = 1; x <= 5; ++x) {
            caves->setChunk({.x = x, .y = 0, .z = 0}, connecting({{kPosX, kNegX}}));
        }
    }
    const auto settle = [&onJob](ChunkPos camera) {
        // The first update starts the job; until a later update takes its
        // result, nothing is hidden.
        CHECK(onJob.update(camera));
        CHECK(onJob.isVisible({.x = 3, .y = 1, .z = 0}));
        while (onJob.isSearching()) {
            onJob.update(camera);
            std::this_thread::yield();
        }
    };
    const auto same = [&] {
        for (i32 z = -2; z <= 2; ++z) {
            for (i32 y = -2; y <= 2; ++y) {
                for (i32 x = -2; x <= 7; ++x) {
                    const ChunkPos position{.x = x, .y = y, .z = z};
                    CHECK(onJob.isVisible(position) == onThread.isVisible(position));
                }
            }
        }
    };
    onThread.update({});
    settle({});
    CHECK_FALSE(onJob.isVisible({.x = 3, .y = 1, .z = 0})); // The rock beside the tunnel.
    same();
    onThread.update({.x = 2, .y = 0, .z = 0});
    settle({.x = 2, .y = 0, .z = 0});
    same();
    // A chunk change searches again on the job too: a cave opening off the
    // tunnel shows the rock above it.
    CHECK_FALSE(onJob.isVisible({.x = 3, .y = 1, .z = 0}));
    onThread.setChunk({.x = 3, .y = 0, .z = 0}, kChunkAllConnected);
    onJob.setChunk({.x = 3, .y = 0, .z = 0}, kChunkAllConnected);
    onThread.update({.x = 2, .y = 0, .z = 0});
    onJob.update({.x = 2, .y = 0, .z = 0});
    while (onJob.isSearching()) {
        onJob.update({.x = 2, .y = 0, .z = 0});
        std::this_thread::yield();
    }
    same();
    CHECK(onJob.isVisible({.x = 3, .y = 1, .z = 0}));
    onJob.setJobSystem(nullptr);
}

TEST_CASE("Cave culling search time over a large world") {
    // A 65 x 16 x 65 chunk world of rock with open air above y 8 and
    // scattered caves: the size of render distance 32.
    CaveCuller caves;
    for (i32 z = -32; z <= 32; ++z) {
        for (i32 x = -32; x <= 32; ++x) {
            for (i32 y = 0; y < 8; ++y) {
                const bool cave = ((x * 7) + (y * 13) + (z * 3)) % 5 == 0;
                caves.setChunk({.x = x, .y = y, .z = z}, cave ? kChunkAllConnected : 0);
            }
        }
    }
    // The first search grows the buffers; later ones, as the camera moves
    // from chunk to chunk, reuse them.
    caves.update({.x = 0, .y = 12, .z = 0});
    const f64 first = caves.lastSearchMilliseconds();
    caves.update({.x = 1, .y = 12, .z = 0});
    MESSAGE("Large world: " << caves.knownChunks() << " known chunks, " << caves.reachedCells()
                            << " reached in " << caves.lastSearchMilliseconds() << " ms (first search "
                            << first << " ms)");
    CHECK(caves.isVisible({.x = 0, .y = 7, .z = 0})); // The surface below the camera.
#if defined(NDEBUG) && !defined(VXE_SANITIZER_ACTIVE)
    if (test::timingIsReliable()) {
        CHECK(caves.lastSearchMilliseconds() < 5.0);
    }
#endif
}

namespace {

std::unique_ptr<ChunkRenderer> makeRenderer(rhi::Device& device, config::SettingsRegistry& settings) {
    auto renderer = ChunkRenderer::create({.device = &device,
                                           .settings = &settings,
                                           .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR),
                                           .arenaBytes = usize{1} << 20U,
                                           .maxChunks = 64,
                                           .stagingBytesPerFrame = usize{1} << 20U});
    if (!renderer) {
        MESSAGE(renderer.error().message());
    }
    REQUIRE(renderer.hasValue());
    // Nothing is drawn without block textures. The Null device frees them with itself.
    constexpr std::array<u32, 1> kStone = {0xFF808080U};
    const auto textures = render::createPlaceholderBlockTextures(device, kStone);
    REQUIRE(textures.hasValue());
    renderer.value()->setTextures(textures.value().texture, textures.value().sampler);
    return std::move(renderer.value());
}

// One opaque quad in each listed face group, so every group is a candidate.
struct Groups {
    std::vector<std::array<u32, 3>> quads; // voxel::PackedQuad: three words.
    std::array<u32, render::kChunkFaceGroups + 1> groupBegin{};

    explicit Groups(std::initializer_list<usize> groups) {
        std::array<u32, render::kChunkFaceGroups> counts{};
        for (const usize group : groups) {
            counts.at(group) = 1;
        }
        for (usize g = 0; g < render::kChunkFaceGroups; ++g) {
            groupBegin.at(g + 1) = groupBegin.at(g) + counts.at(g);
        }
        quads.assign(groupBegin.back(), {});
    }
    [[nodiscard]] render::ChunkGeometry at(ChunkPos position, u16 connectivity = kChunkAllConnected) const {
        return {.position = position,
                .quads = std::as_bytes(std::span(quads)),
                .groupBegin = groupBegin,
                .connectivity = connectivity};
    }
};

// Runs culling for one frame and returns the opaque draw count it recorded.
u32 cullFrame(rhi::Device& device, ChunkRenderer& renderer, const render::Camera& camera) {
    renderer.beginFrame(camera);
    rhi::CommandList commands;
    renderer.recordUploads(commands);
    renderer.recordCulling(commands);
    u32 drawn = 0;
    rhi::CommandList draws;
    renderer.record(draws, ChunkPass::Opaque);
    for (const rhi::CommandView command : draws) {
        if (command.type == rhi::CommandType::MultiDrawIndexedIndirect) {
            drawn = command.as<rhi::cmd::MultiDrawIndexedIndirect>().drawCount;
        }
    }
    device.beginFrame();
    device.submit(commands);
    device.present();
    return drawn;
}

} // namespace

TEST_CASE("CPU culling skips chunks outside the view and faces pointing away") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeRenderer(*device, settings);
    // Camera at the origin looking along -Z. Chunk A is in front, chunk B
    // behind; each has a +Z and a -Z face group.
    const Groups groups({4, 5});
    renderer->beginFrame({});
    REQUIRE(renderer->upload(groups.at({.x = -1, .y = -1, .z = -3})) == UploadResult::Uploaded);
    REQUIRE(renderer->upload(groups.at({.x = -1, .y = -1, .z = 3})) == UploadResult::Uploaded);
    CHECK(cullFrame(*device, *renderer, {}) == 1);
    const render::ChunkCullingCounts& counts = renderer->cullingCounts();
    CHECK_FALSE(counts.onGpu);
    CHECK(counts.candidates == 4);
    CHECK(counts.visible == 1);   // A's +Z faces look at the camera.
    CHECK(counts.direction == 1); // A's -Z faces look away.
    CHECK(counts.frustum == 2);   // B is behind the camera.

    // Each test can be switched off live.
    REQUIRE(settings.set("Graphics.Terrain.FaceDirectionCulling", config::SettingValue{false}).hasValue());
    REQUIRE(settings.set("Graphics.Culling.Frustum", config::SettingValue{false}).hasValue());
    CHECK(cullFrame(*device, *renderer, {}) == 4);
    CHECK(renderer->cullingCounts().visible == 4);
    CHECK(device->stats().invalidCommands == 0);
}

TEST_CASE("CPU culling hides chunks behind a solid wall, and empty meshes count for caves") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeRenderer(*device, settings);
    const Groups front({4});
    render::Camera camera;
    camera.position = {8.0, 8.0, 8.0};
    renderer->beginFrame(camera);
    // A solid chunk with nothing to draw between the camera and chunk C.
    REQUIRE(renderer->upload(Groups({}).at({.x = 0, .y = 0, .z = -1}, 0)) == UploadResult::Uploaded);
    CHECK_FALSE(renderer->isResident({.x = 0, .y = 0, .z = -1}));
    REQUIRE(renderer->upload(front.at({.x = 0, .y = 0, .z = -2})) == UploadResult::Uploaded);
    CHECK(cullFrame(*device, *renderer, camera) == 0);
    CHECK(renderer->cullingCounts().cave == 1);

    // Opening the wall shows the chunk again.
    renderer->beginFrame(camera);
    REQUIRE(renderer->upload(Groups({}).at({.x = 0, .y = 0, .z = -1})) == UploadResult::Uploaded);
    CHECK(cullFrame(*device, *renderer, camera) == 1);
    CHECK(renderer->cullingCounts().cave == 0);

    // So does forgetting it, and switching cave culling off.
    REQUIRE(renderer->upload(Groups({}).at({.x = 0, .y = 0, .z = -1}, 0)) == UploadResult::Uploaded);
    CHECK(cullFrame(*device, *renderer, camera) == 0);
    CHECK_FALSE(renderer->remove({.x = 0, .y = 0, .z = -1})); // Known, but not drawn.
    CHECK(cullFrame(*device, *renderer, camera) == 1);
    REQUIRE(renderer->upload(Groups({}).at({.x = 0, .y = 0, .z = -1}, 0)) == UploadResult::Uploaded);
    REQUIRE(settings.set("Graphics.Culling.CaveCulling", config::SettingValue{false}).hasValue());
    CHECK(cullFrame(*device, *renderer, camera) == 1);
}

TEST_CASE("Frozen culling keeps the camera it had") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeRenderer(*device, settings);
    const Groups groups({4, 5});
    renderer->beginFrame({});
    REQUIRE(renderer->upload(groups.at({.x = -1, .y = -1, .z = -3})) == UploadResult::Uploaded);
    CHECK(cullFrame(*device, *renderer, {}) == 1);
    REQUIRE(settings.set("Debug.FreezeCulling", config::SettingValue{true}).hasValue());
    CHECK(cullFrame(*device, *renderer, {}) == 1);
    REQUIRE(renderer->frozenCullingCamera().has_value());
    CHECK(renderer->cullingSummary().find("frozen") != std::string::npos);

    // Turned around, the chunk is behind the camera, but culling still sees
    // it from where it was frozen.
    render::Camera turned;
    turned.yaw = std::numbers::pi_v<f32>;
    CHECK(cullFrame(*device, *renderer, turned) == 1);
    REQUIRE(settings.set("Debug.FreezeCulling", config::SettingValue{false}).hasValue());
    CHECK(cullFrame(*device, *renderer, turned) == 0);
    CHECK_FALSE(renderer->frozenCullingCamera().has_value());
}

TEST_CASE("Without recordCulling every candidate is drawn") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeRenderer(*device, settings);
    const Groups groups({4, 5});
    renderer->beginFrame({});
    REQUIRE(renderer->upload(groups.at({.x = -1, .y = -1, .z = 3})) == UploadResult::Uploaded);
    rhi::CommandList commands;
    renderer->recordUploads(commands);
    rhi::CommandList draws;
    renderer->record(draws, ChunkPass::Opaque);
    u32 drawn = 0;
    for (const rhi::CommandView command : draws) {
        if (command.type == rhi::CommandType::MultiDrawIndexedIndirect) {
            drawn = command.as<rhi::cmd::MultiDrawIndexedIndirect>().drawCount;
        }
    }
    CHECK(drawn == 2);
}

TEST_CASE("Depth pyramid levels cover the depth buffer with power of two texels") {
    CHECK(render::HiZPyramid::validTexels(1920, 0) == 960);
    CHECK(render::HiZPyramid::validTexels(1080, 0) == 540);
    CHECK(render::HiZPyramid::validTexels(1080, 3) == 68); // ceil(1080 / 16)
    CHECK(render::HiZPyramid::validTexels(1920, 10) == 1);
    CHECK(render::HiZPyramid::validTexels(1, 0) == 1);
    const auto device = rhi::createNullDevice();
    auto pyramid = render::HiZPyramid::create(
        {.device = device.get(), .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
    REQUIRE(pyramid.hasValue());
    CHECK_FALSE(pyramid.value()->hasData());
    const auto depth =
        device->createTexture({.format = rhi::Format::D32Float,
                               .width = 1920,
                               .height = 1080,
                               .usage = rhi::TextureUsage::DepthTarget | rhi::TextureUsage::Sampled});
    REQUIRE(depth.hasValue());
    REQUIRE(pyramid.value()->ensureSize(1920, 1080).hasValue());
    rhi::CommandList commands;
    pyramid.value()->record(commands, depth.value());
    device->submit(commands);
    CHECK(device->stats().invalidCommands == 0);
    CHECK(pyramid.value()->hasData());
    CHECK(pyramid.value()->levels() == 11);
    MESSAGE("Depth pyramid for 1920 x 1080: " << pyramid.value()->memoryBytes() / 1024 << " KiB");
    CHECK(pyramid.value()->memoryBytes() < usize{6} << 20U);
    device->destroy(depth.value());
}

namespace {

// A chunk of stone, or with `mixed` of blocks of every render pass.
voxel::ChunkMesh solidMesh(ChunkPos position, bool mixed = false) {
    voxel::BlockRegistryBuilder builder;
    for (const voxel::RenderLayer layer : {voxel::RenderLayer::Opaque, voxel::RenderLayer::Cutout,
                                           voxel::RenderLayer::Plant, voxel::RenderLayer::Translucent}) {
        voxel::BlockRenderInfo info{.layer = layer};
        info.faceTextureLayer.fill(0);
        REQUIRE(builder.add(info).hasValue());
    }
    const auto registry = builder.build();
    auto snapshot = std::make_unique<voxel::ChunkSnapshot>();
    snapshot->position = position;
    snapshot->light.fill(voxel::kOpenSkyLight); // Lit like open sky, as unlit chunks are.
    for (i32 y = 0; y < 16; ++y) {
        for (i32 z = 0; z < 16; ++z) {
            for (i32 x = 0; x < 16; ++x) {
                snapshot->states[voxel::ChunkSnapshot::index(x, y, z)] =
                    mixed ? static_cast<voxel::BlockStateId>(1 + ((x + (y * 2) + (z * 3)) % 4))
                          : voxel::BlockStateId{1};
            }
        }
    }
    voxel::Mesher mesher;
    voxel::ChunkMesh mesh;
    mesher.mesh(*snapshot, *registry, {}, mesh);
    return mesh;
}

render::ChunkGeometry geometryOf(const voxel::ChunkMesh& mesh) {
    return {.position = mesh.position,
            .quads = std::as_bytes(std::span(mesh.quads)),
            .groupBegin = mesh.groupBegin,
            .connectivity = mesh.connectivity,
            .bounds = {.min = mesh.boundsMin, .max = mesh.boundsMax}};
}

} // namespace

TEST_CASE("GPU culling matches the CPU path and hides chunks behind walls (VXE_GUI_TESTS=1)") {
    if (!test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = platform::Platform::create();
    REQUIRE(platform.hasValue());
    auto window = platform::Window::create(
        *platform.value(), {.title = "vxe culling test", .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto created = rhi::createGlDevice(*window.value(), {.debugOutput = true});
    REQUIRE(created.hasValue());
    rhi::Device& device = *created.value();
    if (!device.capabilities().multiDrawIndirectCount) {
        MESSAGE("No indirect count draws on this driver; GPU culling is not used");
        return;
    }
    {
        config::SettingsRegistry settings;
        auto renderer = render::Renderer::create({.device = &device,
                                                  .settings = &settings,
                                                  .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR)});
        REQUIRE(renderer.hasValue());
        render::Renderer& r = *renderer.value();
        constexpr std::array<u32, 1> kStone = {0xFF808080U};
        auto textures = render::createPlaceholderBlockTextures(device, kStone);
        REQUIRE(textures.hasValue());
        r.chunks().setTextures(textures.value().texture, textures.value().sampler);

        // A 7 x 7 wall of solid chunks at z -1, chunk C behind it at z -3,
        // and a ring of chunks around and behind the camera.
        render::Camera camera;
        camera.position = {8.0, 8.0, 8.0};
        const auto frames = [&](int count) {
            for (int i = 0; i < count; ++i) {
                r.beginFrame(camera, {.deltaSeconds = 0.016});
                REQUIRE(r.endFrame({.deltaSeconds = 0.016}).hasValue());
            }
        };
        r.beginFrame(camera, {});
        for (i32 y = -3; y <= 3; ++y) {
            for (i32 x = -3; x <= 3; ++x) {
                REQUIRE(r.chunks().upload(geometryOf(solidMesh({.x = x, .y = y, .z = -1}))) ==
                        UploadResult::Uploaded);
            }
        }
        for (const ChunkPos position :
             {ChunkPos{.x = 0, .y = 0, .z = -3}, ChunkPos{.x = 2, .y = 0, .z = 2},
              ChunkPos{.x = -3, .y = 1, .z = 1}, ChunkPos{.x = 0, .y = -2, .z = 3}}) {
            REQUIRE(r.chunks().upload(geometryOf(solidMesh(position))) == UploadResult::Uploaded);
        }
        // Every pass in view: its draws must reach the GPU culling too.
        REQUIRE(r.chunks().upload(geometryOf(solidMesh({.x = 0, .y = 0, .z = 0}, true))) ==
                UploadResult::Uploaded);
        REQUIRE(r.endFrame({}).hasValue());

        // Without occlusion both paths must agree exactly.
        REQUIRE(settings.set("Graphics.Culling.Occlusion", config::SettingValue{false}).hasValue());
        frames(8);
        const render::ChunkCullingCounts gpu = r.chunks().cullingCounts();
        REQUIRE(settings.set("Graphics.Culling.Gpu", config::SettingValue{false}).hasValue());
        frames(1);
        const render::ChunkCullingCounts cpu = r.chunks().cullingCounts();
        MESSAGE("GPU: " << gpu.visible << " visible, " << gpu.frustum << " frustum, " << gpu.direction
                        << " direction, " << gpu.cave << " cave");
        CHECK(gpu.onGpu);
        CHECK_FALSE(cpu.onGpu);
        CHECK(gpu.candidates == cpu.candidates);
        CHECK(gpu.visible == cpu.visible);
        CHECK(gpu.frustum == cpu.frustum);
        CHECK(gpu.direction == cpu.direction);
        CHECK(gpu.cave == cpu.cave);
        CHECK(gpu.cave >= 5); // C's groups behind the wall.

        // With caves off, occlusion finds C behind the wall instead.
        REQUIRE(settings.set("Graphics.Culling.Gpu", config::SettingValue{true}).hasValue());
        REQUIRE(settings.set("Graphics.Culling.CaveCulling", config::SettingValue{false}).hasValue());
        REQUIRE(settings.set("Graphics.Culling.Occlusion", config::SettingValue{true}).hasValue());
        frames(8);
        const render::ChunkCullingCounts occluded = r.chunks().cullingCounts();
        MESSAGE(r.chunks().cullingSummary());
        CHECK(occluded.cave == 0);
        CHECK(occluded.occlusion >= 5);
        CHECK(occluded.visible + occluded.frustum + occluded.direction + occluded.occlusion ==
              occluded.candidates);

        CHECK(device.stats().apiErrors == 0);
        CHECK(device.stats().invalidCommands == 0);
        device.destroy(textures.value().texture);
        device.destroy(textures.value().sampler);
    }
    device.waitIdle();
    CHECK(device.liveResources().empty());
}
