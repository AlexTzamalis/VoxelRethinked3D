#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/render/frame_constants.hpp>
#include <vxe/rhi_gl/gl_device.hpp>
#include <vxe/rhi_null/null_device.hpp>
#include <vxe/voxel/mesher.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <vector>

using namespace vxe;
using render::ChunkPass;
using render::ChunkRenderer;
using UploadResult = ChunkRenderer::UploadResult;

namespace {

// The indirect draw layout the renderer writes (DrawElementsIndirectCommand).
struct Draw {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    i32 baseVertex;
    u32 baseInstance;
};

std::unique_ptr<ChunkRenderer> makeRenderer(rhi::Device& device, config::SettingsRegistry& settings,
                                            stats::StatRegistry* stats = nullptr,
                                            usize arenaBytes = usize{1} << 20U,
                                            rhi::Format colorFormat = rhi::Format::RGBA16Float) {
    auto renderer = ChunkRenderer::create({.device = &device,
                                           .settings = &settings,
                                           .stats = stats,
                                           .shaders = rhi::folderShaderReader(VXE_ENGINE_SHADER_DIR),
                                           .colorFormat = colorFormat,
                                           .arenaBytes = arenaBytes,
                                           .maxChunks = 64,
                                           .stagingBytesPerFrame = usize{1} << 20U});
    if (!renderer) {
        MESSAGE(renderer.error().message());
    }
    REQUIRE(renderer.hasValue());
    return std::move(renderer.value());
}

// A mesh of `quads` dummy quads per listed group (pass * 6 + face).
struct FakeMesh {
    std::vector<u64> quads;
    std::array<u32, render::kChunkFaceGroups + 1> groupBegin{};

    FakeMesh(std::initializer_list<std::pair<usize, u32>> groups) {
        std::array<u32, render::kChunkFaceGroups> counts{};
        for (const auto& [group, count] : groups) {
            counts.at(group) = count;
        }
        for (usize g = 0; g < render::kChunkFaceGroups; ++g) {
            groupBegin.at(g + 1) = groupBegin.at(g) + counts.at(g);
        }
        quads.assign(groupBegin.back(), 0x0102030405060708ULL);
    }
    [[nodiscard]] render::ChunkGeometry at(ChunkPos position) const {
        return {.position = position, .quads = std::as_bytes(std::span(quads)), .groupBegin = groupBegin};
    }
};

std::vector<Draw> readDraws(rhi::Device& device, const ChunkRenderer& renderer) {
    const u32 total = renderer.drawCount(ChunkPass::Opaque) + renderer.drawCount(ChunkPass::Cutout) +
                      renderer.drawCount(ChunkPass::Translucent);
    std::vector<Draw> draws(total);
    if (total > 0) {
        REQUIRE(
            device.readBuffer(renderer.drawBuffer(), 0, std::as_writable_bytes(std::span(draws))).hasValue());
    }
    return draws;
}

void frame(rhi::Device& device, ChunkRenderer& renderer, const render::Camera& camera = {}) {
    renderer.beginFrame(camera);
    rhi::CommandList commands;
    renderer.recordUploads(commands);
    device.beginFrame();
    device.submit(commands);
    device.present();
}

} // namespace

TEST_CASE("Uploads build one indirect draw per non empty face group") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    auto renderer = makeRenderer(*device, settings, &stats);
    const FakeMesh mesh(
        {{0, 10}, {2, 5}, {6 + 3, 2}, {12 + 4, 7}}); // Two opaque, one cutout, one translucent.

    renderer->beginFrame({});
    REQUIRE(renderer->upload(mesh.at({.x = 1, .y = 2, .z = 3})) == UploadResult::Uploaded);
    CHECK(renderer->isResident({.x = 1, .y = 2, .z = 3}));
    CHECK(renderer->residentQuads() == 24);
    rhi::CommandList commands;
    renderer->recordUploads(commands);
    device->beginFrame();
    device->submit(commands);
    device->present();
    CHECK(device->stats().invalidCommands == 0);

    CHECK(renderer->drawCount(ChunkPass::Opaque) == 2);
    CHECK(renderer->drawCount(ChunkPass::Cutout) == 1);
    CHECK(renderer->drawCount(ChunkPass::Translucent) == 1);
    const std::vector<Draw> draws = readDraws(*device, *renderer);
    REQUIRE(draws.size() == 4);
    // Group 0 starts the chunk's range, group 2 follows its 10 quads.
    CHECK(draws[0].indexCount == 60);
    CHECK(draws[1].indexCount == 30);
    CHECK(draws[1].baseVertex - draws[0].baseVertex == 40);
    CHECK(draws[2].baseVertex - draws[0].baseVertex == 60);
    CHECK(draws[3].indexCount == 42);
    for (const Draw& draw : draws) {
        CHECK(draw.instanceCount == 1);
        CHECK(draw.baseInstance == draws[0].baseInstance); // All from the same chunk slot.
    }
    // The chunk's position is in its metadata slot, with its bounds packed into w.
    std::array<i32, 4> info{};
    REQUIRE(device
                ->readBuffer(renderer->metadataBuffer(), usize{draws[0].baseInstance} * 16,
                             std::as_writable_bytes(std::span(info)))
                .hasValue());
    CHECK(info == std::array<i32, 4>{1, 2, 3, std::bit_cast<i32>(render::ChunkBounds{}.pack())});

    stats.endFrame(0.016);
    CHECK(stats.value(stats.find("Render.Chunks.Resident")) == 1.0);
    CHECK(stats.value(stats.find("Render.Chunks.Quads")) == 24.0);
    CHECK(stats.value(stats.find("Render.Chunks.UploadBytes")) == 24.0 * 8.0);
    CHECK(stats.value(stats.find("Render.Chunks.BytesPerChunk")) == 24.0 * 8.0 + 16.0);
}

TEST_CASE("The upload budget spreads a burst over frames") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeRenderer(*device, settings);
    REQUIRE(settings.set("World.Streaming.MaxUploadKBPerFrame", config::SettingValue{64}).hasValue());
    const FakeMesh mesh({{0, 1024}}); // 8 KB each.
    i32 next = 0;
    std::vector<usize> perFrame;
    while (next < 20) {
        renderer->beginFrame({});
        usize uploaded = 0;
        while (next < 20 &&
               renderer->upload(mesh.at({.x = next, .y = 0, .z = 0})) == UploadResult::Uploaded) {
            ++next;
            ++uploaded;
        }
        CHECK(renderer->uploadedBytesThisFrame() <= 64 * 1024);
        perFrame.push_back(uploaded);
        frame(*device, *renderer);
    }
    CHECK(perFrame == std::vector<usize>{8, 8, 4});
    CHECK(renderer->residentChunks() == 20);
}

TEST_CASE("Replaced and removed chunks free their memory once the GPU is done") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeRenderer(*device, settings);
    const FakeMesh small({{0, 100}});
    const FakeMesh large({{0, 300}});
    renderer->beginFrame({});
    REQUIRE(renderer->upload(small.at({})) == UploadResult::Uploaded);
    CHECK(renderer->arenaUsedBytes() == 800);
    frame(*device, *renderer);

    renderer->beginFrame({});
    REQUIRE(renderer->upload(large.at({})) == UploadResult::Uploaded); // Replaces the chunk.
    CHECK(renderer->residentChunks() == 1);
    CHECK(renderer->residentQuads() == 300);
    CHECK(renderer->arenaUsedBytes() == 3200); // The old range waits for frames in flight.
    frame(*device, *renderer);
    for (int i = 0; i < 3; ++i) {
        frame(*device, *renderer);
    }
    CHECK(renderer->arenaUsedBytes() == 2400);

    // An empty mesh stops drawing the chunk.
    renderer->beginFrame({});
    REQUIRE(renderer->upload(FakeMesh({}).at({})) == UploadResult::Uploaded);
    CHECK_FALSE(renderer->isResident({}));
    CHECK_FALSE(renderer->remove({}));
    for (int i = 0; i < 4; ++i) {
        frame(*device, *renderer);
    }
    CHECK(renderer->arenaUsedBytes() == 0);
    CHECK(renderer->drawCount(ChunkPass::Opaque) == 0);
}

TEST_CASE("A full arena refuses uploads instead of failing") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    auto renderer = makeRenderer(*device, settings, &stats, usize{8} * 100);
    renderer->beginFrame({});
    CHECK(renderer->upload(FakeMesh({{0, 80}}).at({})) == UploadResult::Uploaded);
    CHECK(renderer->upload(FakeMesh({{0, 80}}).at({.x = 1, .y = 0, .z = 0})) == UploadResult::OutOfMemory);
    CHECK_FALSE(renderer->isResident({.x = 1, .y = 0, .z = 0}));
    frame(*device, *renderer);
    stats.endFrame(0.016);
    CHECK(stats.value(stats.find("Render.Chunks.UploadsOutOfMemory")) == 1.0);
}

TEST_CASE("Translucent draws go far to near and follow the camera") {
    const auto device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    auto renderer = makeRenderer(*device, settings);
    const FakeMesh water({{12 + 2, 1}});
    renderer->beginFrame({});
    for (i32 x = 0; x < 4; ++x) {
        REQUIRE(renderer->upload(water.at({.x = x, .y = 0, .z = 0})) == UploadResult::Uploaded);
    }
    frame(*device, *renderer);
    const auto slotOrder = [&] {
        std::vector<u32> slots;
        for (const Draw& draw : readDraws(*device, *renderer)) {
            slots.push_back(draw.baseInstance);
        }
        return slots;
    };
    // Camera in chunk 0: chunk 3 is drawn first.
    const std::vector<u32> fromWest = slotOrder();
    REQUIRE(fromWest.size() == 4);
    render::Camera east;
    east.position = {3 * 16.0 + 8.0, 8.0, 8.0};
    frame(*device, *renderer, east);
    std::vector<u32> fromEast = slotOrder();
    std::ranges::reverse(fromEast);
    CHECK(fromEast == fromWest);
}

namespace {

// Meshes a chunk of stone blocks: the whole chunk, or only `blocks` (chunk
// local positions) in air.
voxel::ChunkMesh stoneMesh(ChunkPos position, std::initializer_list<std::array<i32, 3>> blocks = {}) {
    voxel::BlockRegistryBuilder builder;
    voxel::BlockRenderInfo stone{.layer = voxel::RenderLayer::Opaque};
    stone.faceTextureLayer.fill(0);
    REQUIRE(builder.add(stone).hasValue());
    const auto registry = builder.build();
    auto snapshot = std::make_unique<voxel::ChunkSnapshot>();
    snapshot->position = position;
    for (i32 y = 0; y < 16; ++y) {
        for (i32 z = 0; z < 16; ++z) {
            for (i32 x = 0; x < 16; ++x) {
                snapshot->states[voxel::ChunkSnapshot::index(x, y, z)] = std::empty(blocks) ? 1 : 0;
            }
        }
    }
    for (const auto& block : blocks) {
        snapshot->states[voxel::ChunkSnapshot::index(block[0], block[1], block[2])] = 1;
    }
    voxel::Mesher mesher;
    voxel::ChunkMesh mesh;
    mesher.mesh(*snapshot, *registry, {}, mesh);
    return mesh;
}

render::ChunkGeometry geometryOf(const voxel::ChunkMesh& mesh) {
    return {.position = mesh.position,
            .quads = std::as_bytes(std::span(mesh.quads)),
            .groupBegin = mesh.groupBegin};
}

// Renders the resident chunks from `camera` into a 64 x 64 RGBA8 target
// cleared to black and returns its pixels.
std::vector<std::byte> renderChunks(rhi::Device& device, ChunkRenderer& renderer,
                                    const render::Camera& camera) {
    constexpr u32 kSize = 64;
    const auto color =
        device.createTexture({.width = kSize,
                              .height = kSize,
                              .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::TransferSrc});
    const auto depth = device.createTexture({.format = rhi::Format::D32Float,
                                             .width = kSize,
                                             .height = kSize,
                                             .usage = rhi::TextureUsage::DepthTarget});
    REQUIRE(color.hasValue());
    REQUIRE(depth.hasValue());
    render::FrameConstantsBuffer constants;
    REQUIRE(constants.create(device).hasValue());
    constants.update(device, render::makeFrameConstants(camera, {.width = kSize, .height = kSize}));
    rhi::CommandList commands;
    renderer.recordUploads(commands);
    rhi::RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0] = {.texture = color.value(), .clearColor = {0.0F, 0.0F, 0.0F, 1.0F}};
    pass.hasDepth = true;
    pass.depth = {.texture = depth.value(), .clearDepth = 0.0F};
    commands.beginRenderPass(pass);
    constants.bind(commands);
    renderer.record(commands, ChunkPass::Opaque);
    commands.endRenderPass();
    device.beginFrame();
    device.submit(commands);
    device.present();
    std::vector<std::byte> pixels(std::size_t{kSize} * kSize * 4);
    REQUIRE(device.readTexture(color.value(), 0, 0, pixels).hasValue());
    constants.destroy(device);
    device.destroy(color.value());
    device.destroy(depth.value());
    return pixels;
}

} // namespace

TEST_CASE("Chunks render with vertex pulling, the same far from the origin (VXE_GUI_TESTS=1)") {
    if (!test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = platform::Platform::create();
    REQUIRE(platform.hasValue());
    auto window = platform::Window::create(*platform.value(),
                                           {.title = "vxe chunk test", .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto created = rhi::createGlDevice(*window.value(), {.debugOutput = true});
    REQUIRE(created.hasValue());
    rhi::Device& device = *created.value();
    {
        config::SettingsRegistry settings;
        auto renderer = makeRenderer(device, settings, nullptr, usize{1} << 20U, rhi::Format::RGBA8Unorm);
        constexpr std::array<u32, 1> kStone = {0xFF808080U};
        auto textures = render::createPlaceholderBlockTextures(device, kStone);
        REQUIRE(textures.hasValue());
        renderer->setTextures(textures.value().texture, textures.value().sampler);
        const auto pixel = [](const std::vector<std::byte>& pixels, usize x, usize y, usize c) {
            return static_cast<int>(pixels[(((y * 64) + x) * 4) + c]);
        };

        // A stone chunk 20 blocks in front of the camera, which looks along -Z
        // at its +Z face. Then the same scene 10 million blocks away.
        std::vector<std::vector<std::byte>> images;
        for (const i32 offset : {0, 625000}) {
            const ChunkPos chunk{.x = offset, .y = 4, .z = offset - 2};
            renderer->beginFrame({});
            REQUIRE(renderer->upload(geometryOf(stoneMesh(chunk))) == UploadResult::Uploaded);
            render::Camera camera;
            camera.position = {(offset * 16.0) + 8.0, 72.0, (offset * 16.0) - 32.0 + 20.0 + 16.0};
            images.push_back(renderChunks(device, *renderer, camera));
            renderer->remove(chunk);
        }
        const auto& near = images[0];
        MESSAGE("Center " << pixel(near, 32, 32, 0) << " " << pixel(near, 32, 32, 1) << " "
                          << pixel(near, 32, 32, 2) << ", corner " << pixel(near, 0, 0, 0));
        // The +Z face: gray stone (linear 0.2) times the side shade 0.65.
        CHECK(pixel(near, 32, 32, 0) > 15);
        CHECK(pixel(near, 32, 32, 0) < 60);
        CHECK(pixel(near, 32, 32, 0) == pixel(near, 32, 32, 2));
        CHECK(pixel(near, 0, 0, 0) == 0); // The chunk does not cover the corner.
        CHECK(images[0] == images[1]);

        // Removed chunks are no longer drawn.
        renderer->beginFrame({});
        render::Camera camera;
        camera.position = {8.0, 72.0, 4.0};
        CHECK(pixel(renderChunks(device, *renderer, camera), 32, 32, 0) == 0);

        // Every face seen head on, with and without the AO diagonal flip:
        // points near all four corners must be drawn, which fails if either
        // triangle of a quad is wound the wrong way and culled.
        for (usize f = 0; f < 6; ++f) {
            const auto face = static_cast<voxel::BlockFace>(f);
            const voxel::FaceAxes axes = voxel::faceAxes(face);
            std::array<i32, 3> normal{};
            normal.at(axes.normal) = axes.positive ? 1 : -1;
            for (const bool flipped : {false, true}) {
                // An occluder diagonally in front of corner 1 (+U, -V) darkens
                // only that corner, which flips the diagonal.
                std::array<i32, 3> occluder = {8 + normal[0], 8 + normal[1], 8 + normal[2]};
                occluder.at(axes.u) += 1;
                occluder.at(axes.v) -= 1;
                const voxel::ChunkMesh mesh =
                    flipped ? stoneMesh({}, {{8, 8, 8}, occluder}) : stoneMesh({}, {{8, 8, 8}});
                bool quadFlipped = false;
                for (const voxel::PackedQuad& quad : mesh.group(voxel::MeshLayer::Opaque, face)) {
                    if (quad.x() <= 9 && quad.y() <= 9 && quad.z() <= 9 && quad.x() >= 8 && quad.y() >= 8 &&
                        quad.z() >= 8) {
                        quadFlipped = quad.flipped();
                    }
                }
                CHECK(quadFlipped == flipped);
                renderer->beginFrame({});
                REQUIRE(renderer->upload(geometryOf(mesh)) == UploadResult::Uploaded);
                render::Camera view;
                view.position = {8.5 + (normal[0] * 3.0), 8.5 + (normal[1] * 3.0), 8.5 + (normal[2] * 3.0)};
                const Vec3 look(static_cast<f32>(-normal[0]), static_cast<f32>(-normal[1]),
                                static_cast<f32>(-normal[2]));
                view.yaw = std::atan2(look.x, -look.z);
                view.pitch = std::clamp(std::asin(look.y), -1.55F, 1.55F);
                const auto image = renderChunks(device, *renderer, view);
                for (const auto& [x, y] : std::array<std::pair<usize, usize>, 5>{
                         {{32, 32}, {26, 26}, {38, 26}, {26, 38}, {38, 38}}}) {
                    INFO("face " << f << (flipped ? " flipped" : "") << " pixel " << x << " " << y);
                    CHECK(pixel(image, x, y, 0) > 5);
                }
                renderer->remove({});
            }
        }

        CHECK(device.stats().apiErrors == 0);
        CHECK(device.stats().invalidCommands == 0);
        renderer.reset();
        device.destroy(textures.value().texture);
        device.destroy(textures.value().sampler);
    }
    device.waitIdle();
    CHECK(device.liveResources().empty());
}
