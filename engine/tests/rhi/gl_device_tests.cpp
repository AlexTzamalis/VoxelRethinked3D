#include "../test_helpers.hpp"

#include <vxe/math/projection.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/rhi/gpu_timer.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/rhi/validation.hpp>
#include <vxe/rhi_gl/gl_device.hpp>

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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
        CHECK(device.value()->liveResources().empty());
    } // The device goes before the window, as createGlDevice requires.
}

namespace {

// Reads engine shaders straight from the source tree.
vxe::Result<std::string> readEngineShader(std::string_view path) {
    const std::ifstream file(std::filesystem::path(VXE_ENGINE_SHADER_DIR) / std::filesystem::path(path),
                             std::ios::binary);
    if (!file) {
        return vxe::Error(vxe::ErrorCode::NotFound, std::string(path));
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

vxe::Result<ShaderHandle> compileShader(Device& device, std::string_view path,
                                        const ShaderFileReader& reader) {
    const ShaderStage stage = shaderStageFromPath(path).value();
    auto source = preprocessShader(path, {.stage = stage, .defines = {}}, reader);
    if (!source) {
        return source.error();
    }
    return device.createShader({.stage = stage,
                                .source = source.value().source,
                                .fileNames = source.value().files,
                                .debugName = path});
}

void exerciseShadersAndPipelines(Device& device) {
    const ShaderFileReader engineShaders = &readEngineShader;
    const ShaderHandle vertex = compileShader(device, "debug/triangle.vert", engineShaders).value();
    const ShaderHandle fragment = compileShader(device, "debug/triangle.frag", engineShaders).value();
    const ShaderHandle compute = compileShader(device, "debug/fill_buffer.comp", engineShaders).value();
    CHECK(device.stats().shaders == 3);

    PipelineDesc opaqueDesc{.vertex = vertex, .fragment = fragment, .debugName = "Test opaque"};
    opaqueDesc.colorFormats[0] = Format::RGBA8Unorm;
    opaqueDesc.colorCount = 1;
    opaqueDesc.depthFormat = Format::D32Float;
    PipelineDesc wireframeDesc = opaqueDesc;
    wireframeDesc.debugName = "Test wireframe";
    wireframeDesc.raster.polygonMode = PolygonMode::Line;
    wireframeDesc.raster.cullMode = CullMode::None;
    PipelineDesc translucentDesc = opaqueDesc;
    translucentDesc.debugName = "Test translucent";
    translucentDesc.depth.writeEnabled = false;
    translucentDesc.blend[0] = BlendState::alphaBlend();

    const PipelineHandle opaque = device.createPipeline(opaqueDesc).value();
    const PipelineHandle wireframe = device.createPipeline(wireframeDesc).value();
    const PipelineHandle translucent = device.createPipeline(translucentDesc).value();
    const PipelineHandle fill = device.createPipeline({.compute = compute, .debugName = "Test fill"}).value();

    // Linked programs no longer need their shaders.
    for (const ShaderHandle shader : {vertex, fragment, compute}) {
        device.destroy(shader);
    }
    CHECK(device.stats().shaders == 0);

    CommandList list;
    for (const PipelineHandle pipeline :
         {opaque, opaque, wireframe, opaque, translucent, translucent, fill, fill}) {
        list.bindPipeline(pipeline);
    }
    device.submit(list);
    DeviceStats stats = device.stats();
    CHECK(stats.pipelineBinds == 8);
    CHECK(stats.pipelineBindsSkipped == 3); // The three repeats.
    CHECK(stats.invalidCommands == 0);
    const vxe::u64 firstChanges = stats.stateChanges;
    CHECK(firstChanges > 0);

    // The second time the GL state is known, so far fewer calls go out
    // (only what differs between neighbors in the list).
    device.submit(list);
    stats = device.stats();
    CHECK(stats.pipelineBinds == 16);
    CHECK(stats.pipelineBindsSkipped == 6);
    const vxe::u64 secondChanges = stats.stateChanges - firstChanges;
    MESSAGE("State calls: " << firstChanges << " for the first pass, " << secondChanges << " for the second");
    CHECK(secondChanges < firstChanges);

    // Binding a destroyed pipeline is reported, not applied.
    device.destroy(wireframe);
    list.reset();
    list.bindPipeline(wireframe);
    device.submit(list);
    CHECK(device.stats().invalidCommands == 1);

    for (const PipelineHandle pipeline : {opaque, translucent, fill}) {
        device.destroy(pipeline);
    }
    stats = device.stats();
    CHECK(stats.pipelines == 0);
    CHECK(stats.apiErrors == 0);
    CHECK(stats.apiWarnings == 0);
}

// A reader for the error test: a type error on line 3 of an include file, and
// a fragment shader that compiles but cannot link.
vxe::Result<std::string> readBrokenShader(std::string_view path) {
    if (path == "broken.frag") {
        return std::string("#include \"include/colors.glsl\"\n"
                           "layout(location = 0) out vec4 outColor;\n"
                           "void main() { outColor = tint(); }\n");
    }
    if (path == "include/colors.glsl") {
        return std::string("#pragma once\n"
                           "// Returns the tint.\n"
                           "vec4 tint() { return vec3(1.0); }\n");
    }
    if (path == "unlinked.frag") {
        return std::string("layout(location = 0) out vec4 outColor;\n"
                           "vec4 missingFunction();\n"
                           "void main() { outColor = missingFunction(); }\n");
    }
    return vxe::Error(vxe::ErrorCode::NotFound, std::string(path));
}

void checkReadableErrors(Device& device) {
    const ShaderFileReader reader = &readBrokenShader;
    const auto broken = compileShader(device, "broken.frag", reader);
    REQUIRE(broken.hasError());
    MESSAGE(broken.error().message());
    CHECK(broken.error().code() == vxe::ErrorCode::ParseError);
    CHECK(broken.error().message().find("fragment shader 'broken.frag' does not compile") !=
          std::string::npos);
    CHECK(broken.error().message().find("include/colors.glsl:3:") != std::string::npos);

    const ShaderHandle vertex = compileShader(device, "debug/triangle.vert", &readEngineShader).value();
    const ShaderHandle unlinked = compileShader(device, "unlinked.frag", reader).value();
    PipelineDesc desc{.vertex = vertex, .fragment = unlinked, .debugName = "Unlinked"};
    desc.depth.testEnabled = false;
    const auto pipeline = device.createPipeline(desc);
    REQUIRE(pipeline.hasError());
    MESSAGE(pipeline.error().message());
    CHECK(pipeline.error().message().starts_with("Pipeline 'Unlinked' does not link:"));

    // Shaders of the wrong stage are refused before OpenGL sees them.
    PipelineDesc wrongStage{.vertex = unlinked, .debugName = "Wrong stage"};
    wrongStage.depth.testEnabled = false;
    CHECK(device.createPipeline(wrongStage).hasError());
    device.destroy(vertex);
    device.destroy(unlinked);
}

// Opens a hidden window with a debug context and runs `body` on a device.
template <typename Body>
void withGlDevice(const char* title, const Body& body) {
    auto platform = Platform::create();
    REQUIRE(platform.hasValue());
    auto window =
        Window::create(*platform.value(), WindowDesc{.title = title, .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto device = createGlDevice(*window.value(), GlDeviceOptions{.debugOutput = true});
    REQUIRE(device.hasValue());
    body(*device.value());
    // A clean run destroys everything it made: the leak report is empty (E6.6).
    CHECK(device.value()->liveResources().empty());
    device.value().reset(); // The device goes before the window.
}

} // namespace

TEST_CASE("OpenGL device compiles shaders, links pipelines and skips redundant state (VXE_GUI_TESTS=1)") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    withGlDevice("vxe OpenGL shader test", [](Device& device) { exerciseShadersAndPipelines(device); });
}

TEST_CASE("OpenGL compile and link errors are readable (VXE_GUI_TESTS=1)") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    withGlDevice("vxe OpenGL error test", [](Device& device) { checkReadableErrors(device); });
}

namespace {

// E6.5: rendering through the full command path, checked pixel by pixel.

constexpr vxe::u32 kTargetSize = 64;

struct TriangleParams {
    vxe::Vec4 tint{1.0F};
    vxe::Vec2 offset{0.0F};
    float scale = 1.0F;
    float depth = 0.5F;
};
static_assert(sizeof(TriangleParams) == 32);

struct CubeParams {
    vxe::Mat4 modelViewProjection{1.0F};
    vxe::Mat4 model{1.0F};
};

constexpr std::array<std::uint16_t, 36> kCubeIndices{0, 4, 6, 0, 6, 2, 5, 1, 3, 5, 3, 7, 0, 1, 5, 0, 5, 4,
                                                     2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};

// The objects the render tests share: a 64x64 color and depth target and
// pipelines for the debug shaders.
struct RenderRig {
    Device& device;
    TextureHandle color;
    TextureHandle depth;
    BufferHandle uniforms;
    BufferHandle cubeIndices;
    PipelineHandle triangle;
    PipelineHandle cube;
    PipelineHandle fill;

    explicit RenderRig(Device& target)
        : device(target) {
        const ShaderFileReader shaders = folderShaderReader(VXE_ENGINE_SHADER_DIR);
        color = device
                    .createTexture({.format = Format::RGBA8Unorm,
                                    .width = kTargetSize,
                                    .height = kTargetSize,
                                    .usage = TextureUsage::RenderTarget | TextureUsage::Sampled |
                                             TextureUsage::TransferSrc,
                                    .debugName = "Test color target"})
                    .value();
        depth = device
                    .createTexture({.format = Format::D32Float,
                                    .width = kTargetSize,
                                    .height = kTargetSize,
                                    .usage = TextureUsage::DepthTarget,
                                    .debugName = "Test depth target"})
                    .value();
        uniforms = device
                       .createBuffer({.sizeBytes = 4096,
                                      .usage = BufferUsage::Uniform | BufferUsage::TransferDst,
                                      .debugName = "Test uniforms"})
                       .value();
        cubeIndices = device
                          .createBuffer({.sizeBytes = sizeof(kCubeIndices),
                                         .usage = BufferUsage::Index | BufferUsage::TransferDst,
                                         .debugName = "Test cube indices"})
                          .value();
        REQUIRE(device.updateBuffer(cubeIndices, 0, std::as_bytes(std::span(kCubeIndices))).hasValue());

        PipelineDesc desc{.debugName = "Test triangle"};
        desc.colorFormats[0] = Format::RGBA8Unorm;
        desc.colorCount = 1;
        desc.depthFormat = Format::D32Float;
        desc.raster.cullMode = CullMode::None;
        triangle = link(shaders, "debug/triangle.vert", "debug/triangle.frag", desc);
        desc.debugName = "Test cube";
        desc.raster.cullMode = CullMode::Back;
        cube = link(shaders, "debug/cube.vert", "debug/cube.frag", desc);
        const ShaderHandle compute = createShaderFromFile(device, "debug/fill_buffer.comp", shaders).value();
        fill = device.createPipeline({.compute = compute, .debugName = "Test fill"}).value();
        device.destroy(compute);
    }

    ~RenderRig() {
        for (const PipelineHandle pipeline : {triangle, cube, fill}) {
            device.destroy(pipeline);
        }
        device.destroy(uniforms);
        device.destroy(cubeIndices);
        device.destroy(color);
        device.destroy(depth);
    }
    RenderRig(const RenderRig&) = delete;
    RenderRig& operator=(const RenderRig&) = delete;
    RenderRig(RenderRig&&) = delete;
    RenderRig& operator=(RenderRig&&) = delete;

    PipelineHandle link(const ShaderFileReader& shaders, std::string_view vertexPath,
                        std::string_view fragmentPath, PipelineDesc desc) {
        desc.vertex = createShaderFromFile(device, vertexPath, shaders).value();
        desc.fragment = createShaderFromFile(device, fragmentPath, shaders).value();
        const PipelineHandle pipeline = device.createPipeline(desc).value();
        device.destroy(desc.vertex);
        device.destroy(desc.fragment);
        return pipeline;
    }

    void beginPass(CommandList& commands) const {
        RenderPassDesc pass;
        pass.colorCount = 1;
        pass.colors[0] = {.texture = color, .clearColor = {0.0F, 0.0F, 0.0F, 1.0F}};
        pass.hasDepth = true;
        pass.depth = {.texture = depth};
        commands.beginRenderPass(pass);
    }

    // Uploads triangle parameters at a 256 byte aligned slot and draws.
    void drawTriangle(CommandList& commands, vxe::usize slot, const TriangleParams& params) const {
        REQUIRE(device.updateBuffer(uniforms, slot * 256, std::as_bytes(std::span(&params, 1))).hasValue());
        commands.bindPipeline(triangle);
        commands.bindUniformBuffer(1, 0, uniforms, slot * 256, sizeof(TriangleParams));
        commands.draw(3);
    }

    void setCubeParams() const {
        const vxe::Mat4 projection = vxe::perspectiveReversedInfinite(1.0F, 1.0F, 0.1F);
        const vxe::Mat4 model = glm::rotate(glm::translate(vxe::Mat4(1.0F), vxe::Vec3(0.0F, 0.0F, -4.0F)),
                                            0.6F, vxe::Vec3(0.3F, 1.0F, 0.2F));
        const CubeParams params{.modelViewProjection = projection * model, .model = model};
        REQUIRE(device.updateBuffer(uniforms, 1024, std::as_bytes(std::span(&params, 1))).hasValue());
    }

    void bindCube(CommandList& commands) const {
        commands.bindPipeline(cube);
        commands.bindUniformBuffer(1, 1, uniforms, 1024, sizeof(CubeParams));
        commands.bindIndexBuffer(cubeIndices, IndexType::Uint16);
    }

    [[nodiscard]] std::vector<std::byte> readColor() const {
        std::vector<std::byte> pixels(std::size_t{kTargetSize} * kTargetSize * 4);
        REQUIRE(device.readTexture(color, 0, 0, pixels).hasValue());
        return pixels;
    }
};

// Pixel (x, y), with y = 0 at the bottom (ADR-016), as 0 to 255 values.
std::array<int, 4> pixelAt(const std::vector<std::byte>& pixels, vxe::u32 x, vxe::u32 y) {
    const std::size_t index = (std::size_t{y} * kTargetSize + x) * 4;
    return {std::to_integer<int>(pixels[index]), std::to_integer<int>(pixels[index + 1]),
            std::to_integer<int>(pixels[index + 2]), std::to_integer<int>(pixels[index + 3])};
}

void checkDepthAndClipSpace(RenderRig& rig) {
    CommandList commands;
    rig.beginPass(commands);
    rig.drawTriangle(commands, 0, {.tint = vxe::Vec4(1.0F), .scale = 1.5F, .depth = 0.5F});
    // Reversed Z: smaller depth is farther, so this one is behind and rejected.
    rig.drawTriangle(commands, 1, {.tint = vxe::Vec4(0.0F), .scale = 1.5F, .depth = 0.25F});
    // Larger depth is nearer, so this one wins.
    rig.drawTriangle(commands, 2, {.tint = vxe::Vec4(0.0F, 1.0F, 0.0F, 1.0F), .scale = 1.5F, .depth = 0.75F});
    commands.endRenderPass();
    rig.device.submit(commands);
    auto pixels = rig.readColor();
    const auto center = pixelAt(pixels, 32, 32);
    CHECK(center[0] == 0);
    CHECK(center[1] > 50);
    CHECK(center[2] == 0);
    CHECK(pixelAt(pixels, 0, 0) == std::array<int, 4>{0, 0, 0, 255}); // The clear color.

    // Clip space depth runs from 0 to 1 (glClipControl, ADR-010), so a
    // triangle at depth -0.5 is clipped away. With OpenGL's default -1 to 1
    // range it would be drawn.
    commands.reset();
    rig.beginPass(commands);
    rig.drawTriangle(commands, 3, {.tint = vxe::Vec4(1.0F), .scale = 1.5F, .depth = -0.5F});
    commands.endRenderPass();
    rig.device.submit(commands);
    pixels = rig.readColor();
    CHECK(pixelAt(pixels, 32, 32) == std::array<int, 4>{0, 0, 0, 255});
}

void checkCubeDraws(RenderRig& rig) {
    rig.setCubeParams();
    CommandList commands;
    rig.beginPass(commands);
    rig.bindCube(commands);
    commands.drawIndexed(36);
    commands.endRenderPass();
    rig.device.submit(commands);
    const auto direct = rig.readColor();
    const auto center = pixelAt(direct, 32, 32);
    CHECK(center[0] + center[1] + center[2] > 60); // A lit face.
    CHECK(pixelAt(direct, 0, 0) == std::array<int, 4>{0, 0, 0, 255});

    // The same cube through an indirect draw must give the identical image.
    const std::array<vxe::u32, 5> command{36, 1, 0, 0,
                                          0}; // count, instances, firstIndex, baseVertex, baseInstance
    const BufferHandle indirect =
        rig.device
            .createBuffer({.sizeBytes = 128, // Room for 4 commands, the count draw's maximum.
                           .usage = BufferUsage::Indirect | BufferUsage::TransferDst,
                           .debugName = "Test indirect commands"})
            .value();
    REQUIRE(rig.device.updateBuffer(indirect, 0, std::as_bytes(std::span(command))).hasValue());
    commands.reset();
    rig.beginPass(commands);
    rig.bindCube(commands);
    commands.multiDrawIndexedIndirect(indirect, 0, 1);
    commands.endRenderPass();
    rig.device.submit(commands);
    CHECK(rig.readColor() == direct);

    // And through an indirect count draw, where the GPU reads the number of
    // draws from a buffer: 1 draws the cube, 0 draws nothing.
    if (rig.device.capabilities().multiDrawIndirectCount) {
        const BufferHandle count =
            rig.device
                .createBuffer({.sizeBytes = 16,
                               .usage = BufferUsage::Indirect | BufferUsage::TransferDst,
                               .debugName = "Test draw count"})
                .value();
        for (const vxe::u32 drawCount : {1U, 0U}) {
            REQUIRE(rig.device.updateBuffer(count, 0, std::as_bytes(std::span(&drawCount, 1))).hasValue());
            commands.reset();
            rig.beginPass(commands);
            rig.bindCube(commands);
            commands.multiDrawIndexedIndirectCount(indirect, 0, count, 0, 4);
            commands.endRenderPass();
            rig.device.submit(commands);
            const auto pixels = rig.readColor();
            if (drawCount == 1) {
                CHECK(pixels == direct);
            } else {
                CHECK(pixelAt(pixels, 32, 32) == std::array<int, 4>{0, 0, 0, 255});
            }
        }
        rig.device.destroy(count);
    } else {
        MESSAGE("Indirect count draws are not supported on this driver; skipped");
    }
    rig.device.destroy(indirect);
}

void checkComputeAndCopies(RenderRig& rig) {
    constexpr vxe::u32 kCount = 1000;
    constexpr vxe::u32 kCapacity = 1024;
    Device& device = rig.device;
    const BufferHandle values = device
                                    .createBuffer({.sizeBytes = std::size_t{kCapacity} * 4,
                                                   .usage = BufferUsage::Storage | BufferUsage::TransferDst |
                                                            BufferUsage::TransferSrc,
                                                   .debugName = "Test fill output"})
                                    .value();
    const BufferHandle copy = device
                                  .createBuffer({.sizeBytes = std::size_t{kCapacity} * 4,
                                                 .usage = BufferUsage::TransferDst | BufferUsage::TransferSrc,
                                                 .debugName = "Test copy"})
                                  .value();
    // Storage starts undefined in OpenGL, so the test zeroes it first.
    const std::vector<vxe::u32> zeros(kCapacity, 0);
    REQUIRE(device.updateBuffer(values, 0, std::as_bytes(std::span(zeros))).hasValue());
    const std::array<vxe::u32, 4> params{1000, kCount, 0, 0}; // baseValue, count, std140 padding
    REQUIRE(device.updateBuffer(rig.uniforms, 2048, std::as_bytes(std::span(params))).hasValue());

    CommandList commands;
    commands.bindPipeline(rig.fill);
    commands.bindStorageBuffer(0, 0, values, 0, 0); // Size 0: the whole buffer.
    commands.bindUniformBuffer(1, 0, rig.uniforms, 2048, 16);
    commands.dispatch(kCapacity / 64);
    commands.resourceBarrier(BarrierScope::StorageData | BarrierScope::TransferData);
    commands.copyBuffer(values, 0, copy, 0, std::size_t{kCapacity} * 4);
    commands.resourceBarrier(BarrierScope::TransferData);
    device.submit(commands);

    std::vector<vxe::u32> result(kCapacity);
    REQUIRE(device.readBuffer(values, 0, std::as_writable_bytes(std::span(result))).hasValue());
    bool filled = true;
    for (vxe::u32 i = 0; i < kCapacity; ++i) {
        filled = filled && result[i] == (i < kCount ? 1000 + i : 0U);
    }
    CHECK(filled);
    std::vector<vxe::u32> copied(kCapacity);
    REQUIRE(device.readBuffer(copy, 0, std::as_writable_bytes(std::span(copied))).hasValue());
    CHECK(copied == result);
    device.destroy(values);
    device.destroy(copy);
}

// One mip level written from another through storage images, and a GPU
// result read through a CpuRead buffer once a fence says the copy is done.
void checkStorageImagesAndReadback(RenderRig& rig) {
    Device& device = rig.device;
    constexpr vxe::u32 kSize = 8;
    const TextureHandle pyramid =
        device
            .createTexture({.format = Format::R32Float,
                            .width = kSize,
                            .height = kSize,
                            .mipLevels = 2,
                            .usage = TextureUsage::Storage | TextureUsage::Sampled |
                                     TextureUsage::TransferDst | TextureUsage::TransferSrc,
                            .debugName = "Test pyramid"})
            .value();
    std::vector<float> level0(std::size_t{kSize} * kSize);
    for (std::size_t i = 0; i < level0.size(); ++i) {
        level0[i] = static_cast<float>((i * 7) % 23);
    }
    REQUIRE(device.uploadTexture(pyramid, {.data = std::as_bytes(std::span(level0))}).hasValue());
    const ShaderFileReader shaders = folderShaderReader(VXE_ENGINE_SHADER_DIR);
    const ShaderHandle shader = createShaderFromFile(device, "debug/image_min.comp", shaders).value();
    const PipelineHandle reduce = device.createPipeline({.compute = shader, .debugName = "Test min"}).value();
    device.destroy(shader);

    CommandList commands;
    commands.bindPipeline(reduce);
    commands.bindStorageImage(1, 0, pyramid, 0, ImageAccess::ReadOnly);
    commands.bindStorageImage(1, 1, pyramid, 1, ImageAccess::WriteOnly);
    commands.dispatch(1);
    commands.resourceBarrier(BarrierScope::StorageImage | BarrierScope::TransferData);
    device.submit(commands);
    std::vector<float> level1(std::size_t{kSize / 2} * (kSize / 2));
    REQUIRE(device.readTexture(pyramid, 1, 0, std::as_writable_bytes(std::span(level1))).hasValue());
    const auto at = [&level0](vxe::u32 x, vxe::u32 y) { return level0[(y * kSize) + x]; };
    bool reduced = true;
    for (vxe::u32 y = 0; y < kSize / 2; ++y) {
        for (vxe::u32 x = 0; x < kSize / 2; ++x) {
            const float expected = std::min(std::min(at(2 * x, 2 * y), at((2 * x) + 1, 2 * y)),
                                            std::min(at(2 * x, (2 * y) + 1), at((2 * x) + 1, (2 * y) + 1)));
            reduced = reduced && level1[(y * (kSize / 2)) + x] == expected;
        }
    }
    CHECK(reduced);

    const BufferHandle source = device
                                    .createBuffer({.sizeBytes = 256,
                                                   .usage = BufferUsage::Storage | BufferUsage::TransferDst |
                                                            BufferUsage::TransferSrc,
                                                   .debugName = "Test readback source"})
                                    .value();
    const BufferHandle readback = device
                                      .createBuffer({.sizeBytes = 256,
                                                     .usage = BufferUsage::CpuRead | BufferUsage::TransferDst,
                                                     .debugName = "Test readback"})
                                      .value();
    const std::vector<std::byte> bytes = pattern(256, 5);
    REQUIRE(device.updateBuffer(source, 0, bytes).hasValue());
    commands.reset();
    commands.copyBuffer(source, 0, readback, 0, 256);
    device.submit(commands);
    const FenceHandle fence = device.insertFence();
    for (int spin = 0; spin < 10000 && !device.isFenceSignaled(fence); ++spin) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    REQUIRE(device.isFenceSignaled(fence));
    device.destroy(fence);
    const std::span<std::byte> mapped = device.mappedRange(readback);
    REQUIRE(mapped.size() == 256);
    CHECK(std::equal(mapped.begin(), mapped.end(), bytes.begin()));

    device.destroy(readback);
    device.destroy(source);
    device.destroy(reduce);
    device.destroy(pyramid);
}

void checkGpuTimer(RenderRig& rig) {
    GpuFrameTimer timer;
    REQUIRE(timer.create(rig.device).hasValue());
    rig.setCubeParams();
    CommandList commands;
    // Results arrive a few frames late by design; give the GPU enough frames.
    for (int frame = 0; frame < 200 && !timer.lastFrameMilliseconds(); ++frame) {
        commands.reset();
        timer.beginFrame(rig.device, commands);
        rig.beginPass(commands);
        rig.bindCube(commands);
        commands.drawIndexed(36, 64); // Some real work for the clock to see.
        commands.endRenderPass();
        timer.endFrame(commands);
        rig.device.submit(commands);
        rig.device.present();
    }
    REQUIRE(timer.lastFrameMilliseconds().has_value());
    MESSAGE("GPU frame time: " << *timer.lastFrameMilliseconds() << " ms");
    CHECK(*timer.lastFrameMilliseconds() >= 0.0);
    CHECK(*timer.lastFrameMilliseconds() < 100.0);
    timer.destroy(rig.device);
}

void checkInvalidCommandsAreSkipped(RenderRig& rig) {
    Device& device = rig.device;
    const vxe::u64 before = device.stats().invalidCommands;
    CommandList commands;
    commands.draw(3); // 1: outside a render pass.
    rig.beginPass(commands);
    commands.dispatch(1);                                    // 2: compute inside a pass.
    commands.bindUniformBuffer(1, 0, rig.uniforms, 4, 16);   // 3: misaligned offset.
    commands.bindStorageBuffer(0, 0, rig.uniforms, 0, 16);   // 4: not a Storage buffer.
    commands.bindUniformBuffer(1, 0, rig.uniforms, 0, 8192); // 5: larger than the buffer.
    commands.drawIndexed(3);                                 // 6: no pipeline bound on this fresh device.
    commands.endRenderPass();
    RenderPassDesc windowWithDepth;
    windowWithDepth.colorCount = 1;
    windowWithDepth.colors[0].swapchain = true;
    windowWithDepth.hasDepth = true;
    windowWithDepth.depth.texture = rig.depth;
    commands.beginRenderPass(windowWithDepth); // 7: the window has no depth buffer.
    commands.beginDebugGroup("Never closed");  // 8: counted at the end of the list.
    device.submit(commands);
    const vxe::u64 invalid = device.stats().invalidCommands - before;
    MESSAGE("Invalid commands reported: " << invalid);
    // doctest runs each SUBCASE on a fresh device, so nothing is bound yet.
    CHECK(invalid == 8);
    // None of them reached OpenGL.
    CHECK(device.stats().apiErrors == 0);
}

} // namespace

TEST_CASE("OpenGL device renders through the full command path (VXE_GUI_TESTS=1)") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    withGlDevice("vxe OpenGL render test", [](Device& device) {
        {
            RenderRig rig(device);
            SUBCASE("depth test and clip space") {
                checkDepthAndClipSpace(rig);
            }
            SUBCASE("indexed, indirect and indirect count draws") {
                checkCubeDraws(rig);
            }
            SUBCASE("compute, barriers and copies") {
                checkComputeAndCopies(rig);
            }
            SUBCASE("storage images and readback buffers") {
                checkStorageImagesAndReadback(rig);
            }
            SUBCASE("GPU frame timer") {
                checkGpuTimer(rig);
            }
            SUBCASE("invalid commands are skipped") {
                checkInvalidCommandsAreSkipped(rig);
            }
        }
        const DeviceStats stats = device.stats();
        CHECK(stats.apiErrors == 0);
        CHECK(stats.apiWarnings == 0);
        CHECK(stats.buffers == 0);
        CHECK(stats.textures == 0);
        CHECK(stats.pipelines == 0);
    });
}

namespace {

// E6.6: destroyed objects stay alive until the GPU has finished their frame.
void checkDeferredDestruction(Device& device) {
    const auto makeBuffer = [&device](std::string_view name) {
        return device
            .createBuffer({.sizeBytes = 256,
                           .usage = BufferUsage::Uniform | BufferUsage::TransferDst,
                           .debugName = name})
            .value();
    };
    // Used by a submitted frame, then destroyed: the handle dies at once, the
    // GL object waits for the frame's fence.
    const BufferHandle used = makeBuffer("Used then destroyed");
    CommandList commands;
    commands.bindUniformBuffer(1, 0, used, 0, 0);
    device.beginFrame();
    device.submit(commands);
    device.destroy(used);
    CHECK(device.stats().buffers == 0);
    CHECK(device.stats().pendingDestroys == 1);
    commands.reset();
    commands.bindUniformBuffer(1, 0, used, 0, 0); // The handle is already invalid.
    device.submit(commands);
    CHECK(device.stats().invalidCommands == 1);

    // Presenting frames lets the GPU catch up; the object is then deleted
    // without anyone waiting for it.
    bool released = false;
    for (int frame = 0; frame < 300 && !released; ++frame) {
        device.present();
        device.beginFrame();
        released = device.stats().pendingDestroys == 0;
        if (!released) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(released);

    // waitIdle releases everything at once, frames or not.
    device.destroy(makeBuffer("Waited for"));
    CHECK(device.stats().pendingDestroys == 1);
    device.waitIdle();
    CHECK(device.stats().pendingDestroys == 0);

    // Unnamed objects still get a name for debuggers and the leak report.
    const BufferHandle unnamed = makeBuffer("");
    const std::vector<std::string> live = device.liveResources();
    REQUIRE(live.size() == 1);
    CHECK(live[0].starts_with("buffer 'unnamed buffer "));
    device.destroy(unnamed);
    CHECK(device.liveResources().empty());
    CHECK(device.stats().apiErrors == 0);
}

} // namespace

TEST_CASE("OpenGL device defers destruction until the GPU is done (VXE_GUI_TESTS=1)") {
    if (!vxe::test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    withGlDevice("vxe OpenGL lifetime test", [](Device& device) { checkDeferredDestruction(device); });
}
