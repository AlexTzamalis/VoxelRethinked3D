#include <vxe/rhi/gpu_timer.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <vector>

using namespace vxe::rhi;

namespace {

// A device with the resources a small renderer would create.
struct Scene {
    std::unique_ptr<Device> device = createNullDevice();
    BufferHandle staging;
    BufferHandle arena;
    BufferHandle indirect;
    TextureHandle blocks;
    TextureHandle depth;
    SamplerHandle sampler;
    ShaderHandle vertex;
    ShaderHandle fragment;
    ShaderHandle cull;
    PipelineHandle terrain;
    PipelineHandle culling;

    Scene() {
        staging = device
                      ->createBuffer({.sizeBytes = 4096,
                                      .usage = BufferUsage::CpuWrite | BufferUsage::TransferSrc,
                                      .debugName = "Staging ring"})
                      .value();
        arena = device
                    ->createBuffer({.sizeBytes = 1 << 20,
                                    .usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                    .debugName = "Vertex arena"})
                    .value();
        indirect = device
                       ->createBuffer({.sizeBytes = 1024,
                                       .usage = BufferUsage::Indirect | BufferUsage::Storage |
                                                BufferUsage::TransferDst | BufferUsage::TransferSrc,
                                       .debugName = "Indirect draws"})
                       .value();
        blocks = device
                     ->createTexture({.type = TextureType::Texture2DArray,
                                      .format = Format::RGBA8Srgb,
                                      .width = 16,
                                      .height = 16,
                                      .layers = 64,
                                      .mipLevels = 5,
                                      .usage = TextureUsage::Sampled | TextureUsage::TransferDst,
                                      .debugName = "Block textures"})
                     .value();
        depth = device
                    ->createTexture({.format = Format::D32Float,
                                     .width = 1920,
                                     .height = 1080,
                                     .usage = TextureUsage::DepthTarget,
                                     .debugName = "Depth"})
                    .value();
        sampler = device->createSampler({.debugName = "Nearest"}).value();
        vertex =
            device
                ->createShader(
                    {.stage = ShaderStage::Vertex, .source = "void main() {}", .debugName = "Terrain vs"})
                .value();
        fragment =
            device
                ->createShader(
                    {.stage = ShaderStage::Fragment, .source = "void main() {}", .debugName = "Terrain fs"})
                .value();
        cull = device
                   ->createShader(
                       {.stage = ShaderStage::Compute, .source = "void main() {}", .debugName = "Cull cs"})
                   .value();
        PipelineDesc terrainDesc{.vertex = vertex, .fragment = fragment, .debugName = "Terrain"};
        terrainDesc.depthFormat = Format::D32Float;
        terrain = device->createPipeline(terrainDesc).value();
        culling = device->createPipeline({.compute = cull, .debugName = "Culling"}).value();
    }

    // Clean teardown, so the leak report stays quiet.
    ~Scene() {
        for (const PipelineHandle p : {terrain, culling}) {
            device->destroy(p);
        }
        for (const ShaderHandle s : {vertex, fragment, cull}) {
            device->destroy(s);
        }
        device->destroy(sampler);
        device->destroy(blocks);
        device->destroy(depth);
        for (const BufferHandle b : {staging, arena, indirect}) {
            device->destroy(b);
        }
    }
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;
};

RenderPassDesc mainPass(TextureHandle depth) {
    RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].swapchain = true;
    pass.hasDepth = true;
    pass.depth.texture = depth;
    return pass;
}

} // namespace

TEST_CASE("Null device stats count resources and their memory") {
    const Scene scene;
    const DeviceStats stats = scene.device->stats();
    CHECK(stats.buffers == 3);
    CHECK(stats.bufferBytes == 4096 + (1 << 20) + 1024);
    CHECK(stats.textures == 2);
    // 16x16 RGBA8 with 5 mips (1024 + 256 + 64 + 16 + 4 bytes) times 64 layers, plus the depth target.
    CHECK(stats.textureBytes == (1364 * 64) + (1920 * 1080 * 4));
    CHECK(stats.shaders == 3);
    CHECK(stats.pipelines == 2);
    CHECK(stats.samplers == 1);
    CHECK(scene.device->capabilities().rendererName == "Null device");
}

TEST_CASE("Mapped staging memory is real and persistent") {
    const Scene scene;
    const std::span<std::byte> mapped = scene.device->mappedRange(scene.staging);
    REQUIRE(mapped.size() == 4096);
    mapped[0] = std::byte{42};
    CHECK(scene.device->mappedRange(scene.staging)[0] == std::byte{42}); // Same memory every call.
    CHECK(scene.device->mappedRange(scene.arena).empty());               // Not a CpuWrite buffer.
}

TEST_CASE("Readback buffers are mapped and storage images are checked") {
    const Scene scene;
    const BufferHandle readback =
        scene.device
            ->createBuffer({.sizeBytes = 64, .usage = BufferUsage::CpuRead | BufferUsage::TransferDst})
            .value();
    CHECK(scene.device->mappedRange(readback).size() == 64);
    const TextureHandle pyramid =
        scene.device
            ->createTexture({.format = Format::R32Float,
                             .width = 64,
                             .height = 32,
                             .mipLevels = 7,
                             .usage = TextureUsage::Storage | TextureUsage::Sampled})
            .value();
    const vxe::u64 before = scene.device->stats().invalidCommands;
    CommandList list;
    list.bindStorageImage(1, 0, pyramid, 6, ImageAccess::WriteOnly);      // Fine: the 1 x 1 level.
    list.bindStorageImage(1, 0, pyramid, 7, ImageAccess::WriteOnly);      // No such level.
    list.bindStorageImage(1, 2, pyramid, 0, ImageAccess::ReadOnly);       // Only 2 image slots per set.
    list.bindStorageImage(1, 0, scene.blocks, 0, ImageAccess::ReadWrite); // Not a Storage texture.
    scene.device->submit(list);
    CHECK(scene.device->stats().invalidCommands - before == 3);
    scene.device->destroy(pyramid);
    scene.device->destroy(readback);
}

TEST_CASE("Uploads are checked like on the GPU") {
    const Scene scene;
    const std::array<std::byte, 16> small{};
    CHECK(scene.device->updateBuffer(scene.indirect, 1012, small).hasError()); // 1012 + 16 is past 1024.
    CHECK(scene.device->updateBuffer(scene.indirect, 1008, small).hasValue());

    CHECK(scene.device->updateBuffer(scene.staging, 0, small).hasError()); // No TransferDst.

    // What was written can be read back.
    std::array<std::byte, 16> pattern{};
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::byte>(i * 3);
    }
    REQUIRE(scene.device->updateBuffer(scene.indirect, 512, pattern).hasValue());
    std::array<std::byte, 16> readBack{};
    REQUIRE(scene.device->readBuffer(scene.indirect, 512, readBack).hasValue());
    CHECK(readBack == pattern);
    CHECK(scene.device->readBuffer(scene.arena, 0, readBack).hasError());       // No TransferSrc.
    CHECK(scene.device->readBuffer(scene.indirect, 1020, readBack).hasError()); // Past the end.

    const std::vector<std::byte> mip0(std::size_t{16} * 16 * 4);
    CHECK(scene.device->uploadTexture(scene.blocks, {.mipLevel = 0, .layer = 63, .data = mip0}).hasValue());
    CHECK(scene.device->uploadTexture(scene.blocks, {.mipLevel = 0, .layer = 64, .data = mip0}).hasError());
    CHECK(scene.device->uploadTexture(scene.blocks, {.mipLevel = 1, .layer = 0, .data = mip0})
              .hasError());                            // Wrong size.
    const std::vector<std::byte> mip4(std::size_t{4}); // 1 x 1 texel of RGBA8.
    CHECK(scene.device->uploadTexture(scene.blocks, {.mipLevel = 4, .layer = 0, .data = mip4}).hasValue());
}

TEST_CASE("Submitted command lists are counted and checked") {
    const Scene scene;
    CommandList list;
    list.beginDebugGroup("Frame");
    list.bindPipeline(scene.culling);
    list.bindStorageBuffer(0, 0, scene.indirect, 0, 1024);
    list.dispatch(64);
    list.resourceBarrier(BarrierScope::IndirectCommands);
    list.beginRenderPass(mainPass(scene.depth));
    list.bindPipeline(scene.terrain);
    list.bindTexture(2, 0, scene.blocks, scene.sampler);
    list.multiDrawIndexedIndirect(scene.indirect, 0, 16);
    list.endRenderPass();
    list.endDebugGroup();
    scene.device->submit(list);
    scene.device->present();

    const DeviceStats stats = scene.device->stats();
    CHECK(stats.commandsSubmitted == 11);
    CHECK(stats.invalidCommands == 0);
    CHECK(stats.framesPresented == 1);
    CHECK(stats.pipelineBinds == 2);
    CHECK(stats.pipelineBindsSkipped == 0);

    // A repeated bind is counted as skipped, like the OpenGL backend filters it.
    list.reset();
    list.bindPipeline(scene.terrain);
    list.bindPipeline(scene.terrain);
    scene.device->submit(list);
    CHECK(scene.device->stats().pipelineBinds == 4);
    CHECK(scene.device->stats().pipelineBindsSkipped == 2);
}

TEST_CASE("Commands that use destroyed resources or break nesting are flagged") {
    const Scene scene;
    const BufferHandle temporary =
        scene.device->createBuffer({.sizeBytes = 64, .usage = BufferUsage::Storage, .debugName = "Temporary"})
            .value();
    scene.device->destroy(temporary);

    CommandList list;
    list.bindStorageBuffer(0, 0, temporary, 0, 64); // Destroyed: invalid.
    list.draw(3);                                   // Outside a render pass: invalid.
    list.beginRenderPass(mainPass(scene.depth));
    list.dispatch(1); // Compute inside a render pass: invalid.
    // The pass is never ended: invalid too.
    scene.device->submit(list);
    CHECK(scene.device->stats().invalidCommands == 4);
}

TEST_CASE("Pipelines need existing shaders of the right stage") {
    const Scene scene;
    PipelineDesc swapped{.vertex = scene.fragment, .fragment = scene.vertex, .debugName = "Swapped"};
    swapped.depthFormat = Format::D32Float;
    CHECK(scene.device->createPipeline(swapped).hasError());

    const ShaderHandle doomed =
        scene.device->createShader({.stage = ShaderStage::Compute, .source = "x", .debugName = "Doomed"})
            .value();
    scene.device->destroy(doomed);
    CHECK(scene.device->createPipeline({.compute = doomed, .debugName = "Stale"}).hasError());
}

TEST_CASE("Fences are signaled at once on the Null device") {
    const Scene scene;
    const FenceHandle fence = scene.device->insertFence();
    CHECK(scene.device->isFenceSignaled(fence));
    scene.device->destroy(fence);
    CHECK_FALSE(scene.device->isFenceSignaled(fence)); // Destroyed fences are not "signaled".
}

TEST_CASE("Textures without TransferDst cannot be uploaded to") {
    const Scene scene;
    const std::vector<std::byte> data(std::size_t{1920} * 1080 * 4);
    CHECK(scene.device->uploadTexture(scene.depth, {.mipLevel = 0, .layer = 0, .data = data}).hasError());
}

TEST_CASE("Textures beyond the device limits are refused") {
    const auto device = createNullDevice();
    CHECK(device->createTexture({.format = Format::RGBA8Unorm, .width = 32768, .height = 4}).hasError());
}

TEST_CASE("The GPU frame timer reads results kFrames frames later and never stalls") {
    const auto device = createNullDevice();
    GpuFrameTimer timer;
    REQUIRE(timer.create(*device).hasValue());
    CommandList list;
    for (std::size_t frame = 0; frame <= GpuFrameTimer::kFrames; ++frame) {
        // Each frame reads the slot written kFrames frames earlier; until the
        // first slot comes around again, nothing has been measured.
        CHECK_FALSE(timer.lastFrameMilliseconds().has_value());
        list.reset();
        timer.beginFrame(*device, list);
        timer.endFrame(list);
        device->submit(list);
    }
    REQUIRE(timer.lastFrameMilliseconds().has_value());
    CHECK(*timer.lastFrameMilliseconds() == 0.0); // The Null device has no GPU clock.
    CHECK(timer.missedResults() == 0);
    timer.destroy(*device);
}

TEST_CASE("Null timestamps and texture readback follow the device rules") {
    const auto device = createNullDevice();
    const QueryHandle query = device->createTimestampQuery().value();
    CHECK_FALSE(device->timestampNanoseconds(query).has_value()); // Never written yet.
    CommandList list;
    list.writeTimestamp(query);
    device->submit(list);
    CHECK(device->timestampNanoseconds(query) == 0U);
    device->destroy(query);
    list.reset();
    list.writeTimestamp(query); // Destroyed: invalid.
    device->submit(list);
    CHECK(device->stats().invalidCommands == 1);

    const TextureHandle readable =
        device
            ->createTexture({.format = Format::RGBA8Unorm,
                             .width = 4,
                             .height = 4,
                             .usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc,
                             .debugName = "Readable"})
            .value();
    const TextureHandle hidden =
        device->createTexture({.format = Format::RGBA8Unorm, .width = 4, .height = 4, .debugName = "Hidden"})
            .value();
    std::vector<std::byte> pixels(std::size_t{4} * 4 * 4, std::byte{0xAB});
    REQUIRE(device->readTexture(readable, 0, 0, pixels).hasValue());
    CHECK(pixels == std::vector<std::byte>(pixels.size(), std::byte{0})); // The Null device keeps no texels.
    CHECK(device->readTexture(hidden, 0, 0, pixels).hasError());          // No TransferSrc.
    CHECK(device->readTexture(readable, 1, 0, pixels).hasError());        // No mip 1.
    pixels.resize(8);
    CHECK(device->readTexture(readable, 0, 0, pixels).hasError()); // Wrong size.
    device->destroy(readable);
    device->destroy(hidden);
}
