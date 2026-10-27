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
                                       .usage = BufferUsage::Indirect | BufferUsage::Storage,
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

TEST_CASE("Uploads are checked like on the GPU") {
    const Scene scene;
    const std::array<std::byte, 16> small{};
    CHECK(scene.device->updateBuffer(scene.indirect, 1012, small).hasError()); // 1012 + 16 is past 1024.
    CHECK(scene.device->updateBuffer(scene.indirect, 1008, small).hasValue());

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

TEST_CASE("Textures beyond the device limits are refused") {
    const auto device = createNullDevice();
    CHECK(device->createTexture({.format = Format::RGBA8Unorm, .width = 32768, .height = 4}).hasError());
}
