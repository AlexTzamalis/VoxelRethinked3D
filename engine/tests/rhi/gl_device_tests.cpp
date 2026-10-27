#include "../test_helpers.hpp"

#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/rhi/validation.hpp>
#include <vxe/rhi_gl/gl_device.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

using namespace vxe::rhi;
using vxe::platform::Platform;
using vxe::platform::Window;
using vxe::platform::WindowDesc;

namespace {

std::vector<std::byte> pattern(std::size_t size, unsigned seed) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 31U + seed) & 0xFFU);
    }
    return bytes;
}

// Exercises every resource type the OpenGL backend creates in E6.3 against a
// real driver. Counts the KHR_debug errors along the way: the acceptance
// criterion for this step is that there are none.
void exerciseDevice(Device& device) {
    const DeviceCapabilities& caps = device.capabilities();
    MESSAGE("Renderer: " << caps.rendererName << ", driver " << caps.driverVersion);
    CHECK(caps.backend == Backend::OpenGL);
    CHECK(caps.debugOutput);
    CHECK_FALSE(caps.rendererName.empty());
    CHECK(caps.apiVersionMajor == 4);
    CHECK(caps.apiVersionMinor >= 5);
    // The minimums OpenGL 4.5 guarantees.
    CHECK(caps.maxTextureSize >= 16384);
    CHECK(caps.maxTextureArrayLayers >= 2048);
    CHECK(caps.maxComputeWorkGroupInvocations >= 1024);
    CHECK(device.backBufferWidth() > 0);

    // A GPU buffer filled from the CPU and read back.
    const BufferHandle storage = device
                                     .createBuffer({.sizeBytes = 4096,
                                                    .usage = BufferUsage::Storage | BufferUsage::TransferDst |
                                                             BufferUsage::TransferSrc,
                                                    .debugName = "Test storage"})
                                     .value();
    const std::vector<std::byte> written = pattern(1024, 7);
    REQUIRE(device.updateBuffer(storage, 2048, written).hasValue());
    std::vector<std::byte> readBack(1024);
    REQUIRE(device.readBuffer(storage, 2048, readBack).hasValue());
    CHECK(readBack == written);

    // A persistently mapped staging buffer: CPU writes through the mapping
    // are visible to the GPU (coherent), which the readback proves.
    const BufferHandle staging = device
                                     .createBuffer({.sizeBytes = 65536,
                                                    .usage = BufferUsage::CpuWrite | BufferUsage::TransferSrc,
                                                    .debugName = "Test staging ring"})
                                     .value();
    const std::span<std::byte> mapped = device.mappedRange(staging);
    REQUIRE(mapped.size() == 65536);
    const std::vector<std::byte> staged = pattern(256, 99);
    std::ranges::copy(staged, mapped.begin() + 1000);
    CHECK(device.mappedRange(staging).data() == mapped.data()); // The mapping is persistent.
    std::vector<std::byte> stagedBack(256);
    REQUIRE(device.readBuffer(staging, 1000, stagedBack).hasValue());
    CHECK(stagedBack == staged);
    CHECK(device.mappedRange(storage).empty());

    // A block texture array with a full mip chain, every level uploaded.
    const TextureDesc arrayDesc{.type = TextureType::Texture2DArray,
                                .format = Format::RGBA8Srgb,
                                .width = 16,
                                .height = 16,
                                .layers = 64,
                                .mipLevels = 5,
                                .usage = TextureUsage::Sampled | TextureUsage::TransferDst,
                                .debugName = "Test block textures"};
    const TextureHandle blocks = device.createTexture(arrayDesc).value();
    for (vxe::u32 layer = 0; layer < arrayDesc.layers; layer += 7) {
        for (vxe::u32 level = 0; level < arrayDesc.mipLevels; ++level) {
            const auto data = pattern(textureLevelBytes(arrayDesc, level), layer + level);
            CHECK(device.uploadTexture(blocks, {.mipLevel = level, .layer = layer, .data = data}).hasValue());
        }
    }
    // Every other format the backend maps, created once each.
    for (const Format format :
         {Format::R8Unorm, Format::RG8Unorm, Format::RGBA8Unorm, Format::BGRA8Unorm, Format::R16Float,
          Format::RG16Float, Format::RGBA16Float, Format::R32Float, Format::RG32Float, Format::RGBA32Float,
          Format::R32Uint, Format::RG32Uint, Format::RGBA32Uint, Format::R11G11B10Float}) {
        const TextureDesc desc{.format = format,
                               .width = 8,
                               .height = 4,
                               .usage = TextureUsage::Sampled | TextureUsage::TransferDst |
                                        TextureUsage::RenderTarget,
                               .debugName = "Test format"};
        const auto texture = device.createTexture(desc);
        REQUIRE(texture.hasValue());
        const auto data = pattern(textureLevelBytes(desc, 0), 1);
        CHECK(device.uploadTexture(texture.value(), {.data = data}).hasValue());
        device.destroy(texture.value());
    }
    for (const Format format : {Format::D32Float, Format::D24UnormS8Uint}) {
        const auto depth = device.createTexture({.format = format,
                                                 .width = 320,
                                                 .height = 180,
                                                 .usage = TextureUsage::DepthTarget,
                                                 .debugName = "Depth"});
        REQUIRE(depth.hasValue());
        device.destroy(depth.value());
    }
    CHECK(device.createTexture({.format = Format::RGBA8Unorm, .width = caps.maxTextureSize + 1}).hasError());

    // Samplers, including anisotropy above what any GPU offers (clamped).
    const SamplerHandle pixelArt = device
                                       .createSampler({.minFilter = Filter::Nearest,
                                                       .magFilter = Filter::Nearest,
                                                       .mipmapMode = MipmapMode::Linear,
                                                       .maxAnisotropy = 64.0F,
                                                       .debugName = "Test pixel art"})
                                       .value();
    const SamplerHandle clamped = device
                                      .createSampler({.addressU = AddressMode::ClampToEdge,
                                                      .addressV = AddressMode::MirroredRepeat,
                                                      .debugName = "Test clamped"})
                                      .value();

    // Fences signal once the GPU has passed them.
    const FenceHandle fence = device.insertFence();
    bool signaled = false;
    for (int i = 0; i < 1000 && !signaled; ++i) {
        signaled = device.isFenceSignaled(fence);
        if (!signaled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(signaled);
    device.destroy(fence);

    device.setPresentMode(PresentMode::Immediate);
    device.present();

    DeviceStats stats = device.stats();
    CHECK(stats.buffers == 2);
    CHECK(stats.bufferBytes == 4096 + 65536);
    CHECK(stats.textures == 1);
    CHECK(stats.textureBytes == textureTotalBytes(arrayDesc));
    CHECK(stats.samplers == 2);
    CHECK(stats.framesPresented == 1);

    for (const BufferHandle buffer : {storage, staging}) {
        device.destroy(buffer);
    }
    device.destroy(blocks);
    device.destroy(pixelArt);
    device.destroy(clamped);
    stats = device.stats();
    CHECK(stats.buffers == 0);
    CHECK(stats.textures == 0);
    CHECK(stats.samplers == 0);
    CHECK(stats.apiErrors == 0);
    CHECK(stats.apiWarnings == 0);
}

} // namespace

TEST_CASE("The OpenGL device needs an OpenGL window") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = Platform::create();
    REQUIRE(platform.hasValue());
    auto window = Window::create(*platform.value(),
                                 WindowDesc{.api = vxe::platform::GraphicsApi::None, .hidden = true});
    REQUIRE(window.hasValue());
    CHECK(createGlDevice(*window.value()).hasError());
}

TEST_CASE("OpenGL device creates resources without driver errors (VXE_GUI_TESTS=1)") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = Platform::create();
    REQUIRE(platform.hasValue());
    auto window = Window::create(*platform.value(), WindowDesc{.title = "vxe OpenGL device test",
                                                               .width = 320,
                                                               .height = 180,
                                                               .debugContext = true,
                                                               .hidden = true});
    REQUIRE(window.hasValue());
    {
        auto device = createGlDevice(*window.value(), GlDeviceOptions{.debugOutput = true});
        REQUIRE(device.hasValue());
        exerciseDevice(*device.value());
    } // The device goes before the window, as createGlDevice requires.
}
