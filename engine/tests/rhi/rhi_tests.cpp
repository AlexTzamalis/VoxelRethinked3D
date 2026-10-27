#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/handles.hpp>
#include <vxe/rhi/validation.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace vxe::rhi;

TEST_CASE("Handles are typed, compact and invalid by default") {
    static_assert(sizeof(BufferHandle) == 4);
    static_assert(!std::is_same_v<BufferHandle, TextureHandle>); // Cannot be mixed up.
    constexpr BufferHandle kEmpty;
    static_assert(!kEmpty.isValid());
}

TEST_CASE("Handle pools detect stale handles after reuse") {
    HandlePool<BufferTag, std::string> pool;
    const BufferHandle a = pool.allocate("first");
    REQUIRE(pool.get(a) != nullptr);
    CHECK(*pool.get(a) == "first");
    CHECK(pool.liveCount() == 1);

    const auto released = pool.release(a);
    REQUIRE(released.has_value());
    CHECK(*released == "first");
    CHECK(pool.get(a) == nullptr); // Use after destroy is caught...

    const BufferHandle b = pool.allocate("second");
    CHECK(b.index == a.index);           // ...even though the slot is reused...
    CHECK(b.generation != a.generation); // ...because its generation moved on.
    CHECK(pool.get(a) == nullptr);
    CHECK(*pool.get(b) == "second");
    CHECK_FALSE(pool.release(a).has_value()); // Releasing a stale handle does nothing.
    CHECK(pool.liveCount() == 1);

    std::vector<std::string> live;
    pool.forEach([&live](BufferHandle, const std::string& value) { live.push_back(value); });
    CHECK(live == std::vector<std::string>{"second"});
}

TEST_CASE("Generations wrap after 256 reuses of one slot") {
    HandlePool<TextureTag, int> pool;
    TextureHandle handle = pool.allocate(0);
    const TextureHandle first = handle;
    for (int i = 1; i <= 256; ++i) {
        static_cast<void>(pool.release(handle));
        handle = pool.allocate(i);
    }
    CHECK(handle.index == first.index);
    CHECK(handle.generation == first.generation); // The documented limit of 8 bit generations.
}

TEST_CASE("Command lists record every command and replay them in order") {
    CommandList list;
    RenderPassDesc pass;
    pass.colorCount = 1;
    pass.colors[0].swapchain = true;
    pass.colors[0].clearColor = {0.1F, 0.2F, 0.3F, 1.0F};
    BufferHandle buffer;
    buffer.index = 7;
    PipelineHandle pipeline;
    pipeline.index = 3;

    list.beginDebugGroup("Opaque chunks");
    list.beginRenderPass(pass);
    list.bindPipeline(pipeline);
    list.bindStorageBuffer(0, 2, buffer, 256, 1024);
    list.drawIndexed(36, 1, 0, 0, 5);
    list.multiDrawIndexedIndirectCount(buffer, 0, buffer, 64, 4096);
    list.endRenderPass();
    list.resourceBarrier(BarrierScope::StorageData | BarrierScope::IndirectCommands);
    list.dispatch(8, 4, 1);
    list.endDebugGroup();
    CHECK(list.commandCount() == 10);

    std::vector<CommandType> types;
    for (const CommandView command : list) {
        types.push_back(command.type);
        switch (command.type) {
        case CommandType::BeginDebugGroup:
            CHECK(command.debugGroupName() == "Opaque chunks");
            break;
        case CommandType::BeginRenderPass: {
            const auto begin = command.as<cmd::BeginRenderPass>();
            CHECK(begin.pass.colorCount == 1);
            CHECK(begin.pass.colors[0].swapchain);
            CHECK(begin.pass.colors[0].clearColor[2] == 0.3F);
            break;
        }
        case CommandType::BindStorageBuffer: {
            const auto bind = command.as<cmd::BindBufferRange>();
            CHECK(bind.slot == 2);
            CHECK(bind.buffer == buffer);
            CHECK(bind.offset == 256);
            CHECK(bind.size == 1024);
            break;
        }
        case CommandType::DrawIndexed:
            CHECK(command.as<cmd::DrawIndexed>().firstInstance == 5);
            break;
        case CommandType::MultiDrawIndexedIndirectCount:
            CHECK(command.as<cmd::MultiDrawIndexedIndirectCount>().maxDraws == 4096);
            break;
        case CommandType::Barrier:
            CHECK(hasFlag(command.as<cmd::Barrier>().scope, BarrierScope::IndirectCommands));
            break;
        case CommandType::Dispatch:
            CHECK(command.as<cmd::Dispatch>().groupsY == 4);
            break;
        default:
            break;
        }
    }
    CHECK(types == std::vector<CommandType>{
                       CommandType::BeginDebugGroup, CommandType::BeginRenderPass, CommandType::BindPipeline,
                       CommandType::BindStorageBuffer, CommandType::DrawIndexed,
                       CommandType::MultiDrawIndexedIndirectCount, CommandType::EndRenderPass,
                       CommandType::Barrier, CommandType::Dispatch, CommandType::EndDebugGroup});

    // Reset keeps the memory for the next frame.
    const std::size_t bytes = list.sizeBytes();
    list.reset();
    CHECK(list.empty());
    CHECK(list.begin() == list.end());
    for (int i = 0; i < 10; ++i) {
        list.draw(3);
    }
    CHECK(list.sizeBytes() < bytes);
}

TEST_CASE("Descriptions that can never work are rejected") {
    CHECK(validate(BufferDesc{.sizeBytes = 0, .usage = BufferUsage::Storage, .debugName = "x"}).hasError());
    CHECK(validate(BufferDesc{.sizeBytes = 64, .usage = BufferUsage::None, .debugName = "x"}).hasError());
    CHECK(validate(BufferDesc{.sizeBytes = 64, .usage = BufferUsage::Storage | BufferUsage::Indirect})
              .hasValue());

    TextureDesc texture{.type = TextureType::Texture2DArray,
                        .format = Format::RGBA8Unorm,
                        .width = 32,
                        .height = 32,
                        .layers = 256,
                        .mipLevels = 6,
                        .usage = TextureUsage::Sampled | TextureUsage::TransferDst,
                        .debugName = "Block textures"};
    CHECK(validate(texture).hasValue());
    texture.mipLevels = 7; // 32 has 6 levels: 32, 16, 8, 4, 2, 1.
    CHECK(validate(texture).hasError());

    const TextureDesc depthAsColor{.format = Format::D32Float, .usage = TextureUsage::RenderTarget};
    CHECK(validate(depthAsColor).hasError());
    const TextureDesc depth{.format = Format::D32Float, .usage = TextureUsage::DepthTarget};
    CHECK(validate(depth).hasValue());
    const TextureDesc twoLayers2D{.type = TextureType::Texture2D, .layers = 2};
    CHECK(validate(twoLayers2D).hasError());

    CHECK(
        validate(ShaderDesc{.stage = ShaderStage::Fragment, .source = "", .debugName = "empty"}).hasError());

    ShaderHandle shader;
    shader.index = 0;
    const PipelineDesc pipeline{.vertex = shader, .compute = shader};
    CHECK(validate(pipeline).hasError()); // Graphics and compute at once.
    PipelineDesc noDepth{.vertex = shader};
    CHECK(validate(noDepth).hasError()); // Depth test on by default needs a depth format.
    noDepth.depthFormat = Format::D32Float;
    CHECK(validate(noDepth).hasValue());
}

TEST_CASE("Texture size helpers") {
    CHECK(fullMipCount(1, 1) == 1);
    CHECK(fullMipCount(16, 16) == 5);
    CHECK(fullMipCount(1920, 1080) == 11);
    const TextureDesc desc{.format = Format::RGBA16Float, .width = 64, .height = 32};
    CHECK(textureLevelBytes(desc, 0) == 64 * 32 * 8);
    CHECK(textureLevelBytes(desc, 6) == 1 * 1 * 8); // Clamped at 1 texel.
    CHECK(bytesPerTexel(Format::RGBA32Float) == 16);
    CHECK(isDepthFormat(Format::D24UnormS8Uint));
}

TEST_CASE("The Null device skeleton creates validated resources") {
    const auto device = createNullDevice();
    CHECK(device->capabilities().backend == Backend::Null);
    const auto buffer =
        device->createBuffer({.sizeBytes = 1024, .usage = BufferUsage::Storage, .debugName = "test"});
    REQUIRE(buffer.hasValue());
    CHECK(buffer.value().isValid());
    CHECK(device->createBuffer({.sizeBytes = 0, .usage = BufferUsage::Storage}).hasError());
    device->destroy(buffer.value());
    device->destroy(buffer.value()); // A second destroy of a stale handle is harmless.
}
