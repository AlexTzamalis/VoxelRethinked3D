#include "../test_helpers.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/core/memory.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/time.hpp>
#include <vxe/platform/platform.hpp>
#include <vxe/platform/window.hpp>
#include <vxe/render/frame_graph.hpp>
#include <vxe/rhi_gl/gl_device.hpp>
#include <vxe/rhi_null/null_device.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace vxe;
using render::BufferAccess;
using render::FrameGraph;
using render::PassContext;
using render::PassType;
using render::TextureRef;
using rhi::CommandType;
using rhi::LoadOp;

namespace {

constexpr render::TransientTextureDesc kColor{.format = rhi::Format::RGBA8Unorm};
constexpr render::TransientTextureDesc kDepth{.format = rhi::Format::D32Float};

const auto kNothing = [](PassContext&) {};

struct Rig {
    std::unique_ptr<rhi::Device> device = rhi::createNullDevice();
    config::SettingsRegistry settings;
    stats::StatRegistry stats;
    FrameGraph graph{{.device = device.get(), .settings = &settings, .stats = &stats}};
    rhi::CommandList commands;

    // Records and submits one frame, returning the recorded commands' types.
    std::vector<rhi::CommandView> frame() {
        commands.reset();
        REQUIRE(graph.execute(commands).hasValue());
        device->beginFrame();
        device->submit(commands);
        device->present();
        stats.endFrame(1.0 / 60.0);
        return {commands.begin(), commands.end()};
    }
};

std::string compileError(FrameGraph& graph) {
    const auto result = graph.compile();
    return result ? std::string() : result.error().message();
}

} // namespace

TEST_CASE("The frame graph rejects broken pass declarations") {
    SUBCASE("reading a texture no earlier pass wrote") {
        Rig rig;
        auto first = rig.graph.addPass({.name = "First", .record = kNothing});
        const TextureRef color = first.createTexture("Color", kColor);
        auto user = rig.graph.addPass({.name = "User", .record = kNothing});
        user.sampleTexture(color);
        user.windowAttachment();
        first.colorAttachment(color);
        // First is declared before User, so this order is valid...
        CHECK(compileError(rig.graph).empty());
    }
    SUBCASE("a read before the write") {
        Rig rig;
        auto user = rig.graph.addPass({.name = "User", .record = kNothing});
        auto writer = rig.graph.addPass({.name = "Writer", .record = kNothing});
        const TextureRef color = writer.createTexture("Color", kColor);
        writer.colorAttachment(color);
        user.sampleTexture(color);
        user.windowAttachment();
        CHECK(compileError(rig.graph) == "Frame graph pass 'User': reads 'Color' before any pass writes it");
    }
    SUBCASE("sampling a texture the pass renders into") {
        Rig rig;
        auto pass = rig.graph.addPass({.name = "Loop", .record = kNothing});
        const TextureRef color = pass.createTexture("Color", kColor);
        pass.colorAttachment(color);
        pass.sampleTexture(color);
        CHECK(compileError(rig.graph) ==
              "Frame graph pass 'Loop': renders into 'Color' and reads it at the same time");
    }
    SUBCASE("loading undefined contents") {
        Rig rig;
        auto pass = rig.graph.addPass({.name = "Load", .record = kNothing});
        pass.colorAttachment(pass.createTexture("Color", kColor), LoadOp::Load);
        CHECK(compileError(rig.graph) == "Frame graph pass 'Load': loads 'Color' before any pass writes it");
    }
    SUBCASE("attachment rules") {
        Rig rig;
        static_cast<void>(rig.graph.addPass({.name = "Empty", .record = kNothing}));
        CHECK(compileError(rig.graph) == "Frame graph pass 'Empty': a raster pass needs an attachment");
    }
    SUBCASE("window plus textures") {
        Rig rig;
        auto pass = rig.graph.addPass({.name = "Mixed", .record = kNothing});
        pass.colorAttachment(pass.createTexture("Color", kColor));
        pass.windowAttachment();
        CHECK(compileError(rig.graph).find("cannot be combined") != std::string::npos);
    }
    SUBCASE("compute with attachments") {
        Rig rig;
        auto pass = rig.graph.addPass({.name = "Cull", .type = PassType::Compute, .record = kNothing});
        pass.windowAttachment();
        CHECK(compileError(rig.graph) == "Frame graph pass 'Cull': only raster passes have attachments");
    }
    SUBCASE("depth formats") {
        Rig rig;
        auto pass = rig.graph.addPass({.name = "Depth", .record = kNothing});
        pass.depthAttachment(pass.createTexture("NotDepth", kColor));
        CHECK(compileError(rig.graph) ==
              "Frame graph pass 'Depth': 'NotDepth' has the wrong format for this attachment");
    }
    SUBCASE("names") {
        Rig rig;
        static_cast<void>(rig.graph.addPass({.name = "Has space", .record = kNothing}));
        CHECK(compileError(rig.graph) ==
              "Frame graph pass 'Has space': names may only use letters and digits");
    }
    SUBCASE("duplicate names and missing record functions") {
        Rig rig;
        rig.graph.addPass({.name = "Same", .record = kNothing}).windowAttachment();
        rig.graph.addPass({.name = "Same", .record = kNothing}).windowAttachment();
        CHECK(compileError(rig.graph) == "Frame graph pass 'Same': the name is used twice");
        Rig other;
        other.graph.addPass({.name = "NoRecord"}).windowAttachment();
        CHECK(compileError(other.graph) == "Frame graph pass 'NoRecord': has no record function");
    }
}

TEST_CASE("Passes run in order inside their render passes") {
    Rig rig;
    std::vector<std::string> ran;
    u32 seenWidth = 0;
    TextureRef scene;
    TextureRef depth;
    TextureRef hdr;
    auto opaque = rig.graph.addPass({.name = "Opaque", .record = [&](PassContext& context) {
                                         ran.emplace_back("Opaque");
                                         CHECK(context.texture(scene).isValid());
                                         seenWidth = context.width;
                                     }});
    scene = opaque.createTexture("Scene", kColor);
    depth = opaque.createTexture("Depth", kDepth);
    opaque.colorAttachment(scene, LoadOp::Clear, {0.1F, 0.2F, 0.3F, 1.0F});
    opaque.depthAttachment(depth);
    auto post =
        rig.graph.addPass({.name = "Post", .record = [&](PassContext&) { ran.emplace_back("Post"); }});
    hdr = post.createTexture("Hdr", {.format = rhi::Format::RGBA16Float, .scale = 0.5F});
    post.sampleTexture(scene);
    post.colorAttachment(hdr);
    auto present = rig.graph.addPass({.name = "Present", .canDisable = false, .record = [&](PassContext&) {
                                          ran.emplace_back("Present");
                                      }});
    present.sampleTexture(hdr);
    present.windowAttachment(LoadOp::DontCare);
    REQUIRE(rig.graph.compile().hasValue());
    CHECK_FALSE(rig.graph.compile().hasValue()); // Only once.
    rig.graph.setOutputSize(320, 200);

    const auto commands = rig.frame();
    CHECK(ran == std::vector<std::string>{"Opaque", "Post", "Present"});
    CHECK(seenWidth == 320);
    CHECK(rig.device->stats().invalidCommands == 0);

    std::vector<std::string> groups;
    std::vector<rhi::RenderPassDesc> passes;
    for (const rhi::CommandView& command : commands) {
        if (command.type == CommandType::BeginDebugGroup) {
            groups.emplace_back(command.debugGroupName());
        } else if (command.type == CommandType::BeginRenderPass) {
            passes.push_back(command.as<rhi::cmd::BeginRenderPass>().pass);
        }
    }
    CHECK(groups == std::vector<std::string>{"Opaque", "Post", "Present"});
    REQUIRE(passes.size() == 3);
    CHECK(passes[0].colorCount == 1);
    CHECK(passes[0].colors[0].texture == rig.graph.texture(scene));
    CHECK(passes[0].colors[0].clearColor[2] == 0.3F);
    CHECK(passes[0].hasDepth);
    CHECK(passes[0].depth.texture == rig.graph.texture(depth));
    CHECK(passes[1].colors[0].texture == rig.graph.texture(hdr));
    CHECK(passes[2].colors[0].swapchain);
    CHECK(passes[2].colors[0].load == LoadOp::DontCare);

    // Sizes follow the output: 320x200 RGBA8 and D32, plus a half size RGBA16F.
    CHECK(rig.graph.physicalTextureCount() == 3);
    CHECK(rig.graph.transientBytes() == 320U * 200 * 4 * 2 + 160U * 100 * 8);
    rig.graph.setOutputSize(640, 400);
    static_cast<void>(rig.frame());
    CHECK(rig.graph.transientBytes() == 640U * 400 * 4 * 2 + 320U * 200 * 8);
    CHECK(rig.device->stats().textures == 3); // The old ones were released.

    // The pass that cannot be disabled has no toggle; the others do.
    CHECK(rig.settings.find("Debug.Passes.Opaque") != nullptr);
    CHECK(rig.settings.find("Debug.Passes.Present") == nullptr);
    CHECK(rig.stats.find("Gpu.Pass.Post").isValid());
    CHECK(rig.stats.value(rig.stats.find("Render.FrameGraph.PassesRun")) == 3.0);
}

TEST_CASE("Transient textures share memory when their passes do not overlap") {
    Rig rig;
    // A ping pong chain: A writes T1, B reads T1 and writes T2, C reads T2
    // and writes T3, D reads T3. T3 can reuse T1, which is dead after B.
    auto a = rig.graph.addPass({.name = "A", .record = kNothing});
    const TextureRef t1 = a.createTexture("T1", kColor);
    a.colorAttachment(t1);
    auto b = rig.graph.addPass({.name = "B", .record = kNothing});
    const TextureRef t2 = b.createTexture("T2", kColor);
    b.sampleTexture(t1);
    b.colorAttachment(t2);
    auto c = rig.graph.addPass({.name = "C", .record = kNothing});
    const TextureRef t3 = c.createTexture("T3", kColor);
    c.sampleTexture(t2);
    c.colorAttachment(t3);
    auto d = rig.graph.addPass({.name = "D", .record = kNothing});
    d.sampleTexture(t3);
    d.windowAttachment();
    REQUIRE(rig.graph.compile().hasValue());
    rig.graph.setOutputSize(64, 64);

    static_cast<void>(rig.frame());
    CHECK(rig.graph.physicalTextureCount() == 2);
    CHECK(rig.graph.texture(t3) == rig.graph.texture(t1));
    CHECK(rig.graph.texture(t2) != rig.graph.texture(t1));

    // Turning aliasing off applies on the next frame.
    REQUIRE(rig.settings.set("Debug.FrameGraph.Aliasing", false).hasValue());
    static_cast<void>(rig.frame());
    CHECK(rig.graph.physicalTextureCount() == 3);
    CHECK(rig.graph.texture(t3) != rig.graph.texture(t1));
    CHECK(rig.device->stats().invalidCommands == 0);
}

TEST_CASE("Barriers follow shader storage writes, also into the next frame") {
    Rig rig;
    const auto drawCommands = rig.device->createBuffer(
        {.sizeBytes = 256, .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect});
    const auto pyramid = rig.device->createTexture(
        {.width = 16, .height = 16, .usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled});
    REQUIRE(drawCommands.hasValue());
    REQUIRE(pyramid.hasValue());
    const render::BufferRef indirect = rig.graph.importBuffer("DrawCommands", drawCommands.value());
    const TextureRef hiZ = rig.graph.importTexture("HiZ", pyramid.value());

    // Like Section 8.1: culling reads last frame's Hi-Z and writes draws,
    // the opaque pass draws them, the Hi-Z build at the end writes the pyramid.
    auto cull = rig.graph.addPass({.name = "Cull", .type = PassType::Compute, .record = kNothing});
    cull.sampleTexture(hiZ);
    cull.useBuffer(indirect, BufferAccess::StorageWrite);
    auto opaque = rig.graph.addPass({.name = "Opaque", .record = kNothing});
    opaque.useBuffer(indirect, BufferAccess::Indirect);
    opaque.colorAttachment(opaque.createTexture("Scene", kColor));
    auto build = rig.graph.addPass({.name = "HiZBuild", .type = PassType::Compute, .record = kNothing});
    build.writeStorageTexture(hiZ);
    REQUIRE(rig.graph.compile().hasValue());

    std::vector<rhi::BarrierScope> barriers;
    for (const rhi::CommandView& command : rig.frame()) {
        if (command.type == CommandType::Barrier) {
            barriers.push_back(command.as<rhi::cmd::Barrier>().scope);
        }
    }
    // Cull: the pyramid written by last frame's build, and its own write of
    // the draws after last frame's write (write after write needs a barrier
    // too). Opaque: the draws Cull wrote. HiZBuild: its write after last
    // frame's write of the pyramid.
    CHECK(barriers == std::vector<rhi::BarrierScope>{
                          rhi::BarrierScope::TextureFetch | rhi::BarrierScope::StorageData,
                          rhi::BarrierScope::IndirectCommands, rhi::BarrierScope::StorageImage});
    CHECK(rig.graph.barrierCount() == 3);
    CHECK(rig.device->stats().invalidCommands == 0);
    rig.device->destroy(drawCommands.value());
    rig.device->destroy(pyramid.value());
}

TEST_CASE("A disabled pass only clears what it would have cleared") {
    Rig rig;
    int sceneRuns = 0;
    auto scenePass = rig.graph.addPass({.name = "Scene", .record = [&](PassContext&) { ++sceneRuns; }});
    const TextureRef color = scenePass.createTexture("Color", kColor);
    scenePass.colorAttachment(color, LoadOp::Clear, {0.0F, 0.0F, 1.0F, 1.0F});
    auto overlay = rig.graph.addPass({.name = "Overlay", .record = kNothing});
    overlay.colorAttachment(color, LoadOp::Load); // Draws on top; clears nothing.
    auto present = rig.graph.addPass({.name = "Present", .canDisable = false, .record = kNothing});
    present.sampleTexture(color);
    present.windowAttachment(LoadOp::DontCare);
    REQUIRE(rig.graph.compile().hasValue());

    REQUIRE(rig.settings.set("Debug.Passes.Scene", false).hasValue());
    REQUIRE(rig.settings.set("Debug.Passes.Overlay", false).hasValue());
    CHECK_FALSE(rig.graph.isPassEnabled(0));
    std::vector<rhi::RenderPassDesc> passes;
    std::vector<std::string> groups;
    for (const rhi::CommandView& command : rig.frame()) {
        if (command.type == CommandType::BeginRenderPass) {
            passes.push_back(command.as<rhi::cmd::BeginRenderPass>().pass);
        } else if (command.type == CommandType::BeginDebugGroup) {
            groups.emplace_back(command.debugGroupName());
        }
    }
    CHECK(sceneRuns == 0);
    CHECK(groups == std::vector<std::string>{"Present"});
    // The scene pass's clear stays, so Present samples blue, not old contents;
    // the overlay pass leaves nothing behind.
    REQUIRE(passes.size() == 2);
    CHECK(passes[0].colors[0].texture == rig.graph.texture(color));
    CHECK(passes[0].colors[0].load == LoadOp::Clear);
    CHECK(passes[0].colors[0].clearColor[2] == 1.0F);
    CHECK(passes[1].colors[0].swapchain);
    CHECK(rig.stats.value(rig.stats.find("Render.FrameGraph.PassesRun")) == 1.0);

    // Back on: live, from the next frame.
    REQUIRE(rig.settings.set("Debug.Passes.Scene", true).hasValue());
    static_cast<void>(rig.frame());
    CHECK(sceneRuns == 1);
}

TEST_CASE("Executing the frame graph does not allocate and stays within budget") {
    Rig rig;
    // Eleven passes like Section 8.1, each with a target, so the cost of the
    // graph itself (not of drawing) is measured.
    TextureRef previous;
    for (int i = 0; i < 11; ++i) {
        auto pass = rig.graph.addPass({.name = "Pass" + std::to_string(i), .record = kNothing});
        if (previous.isValid()) {
            pass.sampleTexture(previous);
        }
        if (i == 10) {
            pass.windowAttachment();
        } else {
            previous = pass.createTexture("Target" + std::to_string(i), kColor);
            pass.colorAttachment(previous);
        }
    }
    REQUIRE(rig.graph.compile().hasValue());
    rig.graph.setOutputSize(1920, 1080);
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(rig.frame()); // Warm up: textures, command list capacity, stat slots.
    }
    constexpr int kFrames = 1000;
    const u64 allocationsBefore = memory::globalAllocationCount();
    const TimePoint start = Clock::now();
    for (int i = 0; i < kFrames; ++i) {
        rig.commands.reset();
        REQUIRE(rig.graph.execute(rig.commands).hasValue());
    }
    const f64 milliseconds = toMilliseconds(Clock::now() - start) / kFrames;
    const u64 allocations = memory::globalAllocationCount() - allocationsBefore;
    MESSAGE("Frame graph execute with 11 passes: " << milliseconds << " ms, " << allocations
                                                   << " allocations in " << kFrames << " frames");
    if (memory::globalAllocationCountAvailable()) {
        CHECK(allocations == 0);
    }
    // Section 8.1 sets no number; the graph must stay far below the overlay's
    // 0.2 ms. Generous here for debug and sanitizer builds.
    CHECK(milliseconds < 0.2);
    CHECK(rig.graph.physicalTextureCount() == 2); // The chain ping pongs between two targets.
}

TEST_CASE("Frame graph passes render and are timed on the GPU (VXE_GUI_TESTS=1)") {
    if (!test::guiTestsEnabled()) {
        MESSAGE("Skipped: set VXE_GUI_TESTS=1 to run tests that open real windows");
        return;
    }
    auto platform = platform::Platform::create();
    REQUIRE(platform.hasValue());
    auto window = platform::Window::create(
        *platform.value(), {.title = "vxe frame graph test", .debugContext = true, .hidden = true});
    REQUIRE(window.hasValue());
    auto created = rhi::createGlDevice(*window.value(), {.debugOutput = true});
    REQUIRE(created.hasValue());
    rhi::Device& device = *created.value();
    {
        config::SettingsRegistry settings;
        stats::StatRegistry stats;
        FrameGraph graph({.device = &device, .settings = &settings, .stats = &stats});
        auto fill = graph.addPass({.name = "Fill", .record = kNothing});
        const TextureRef color = fill.createTexture(
            "Color", {.width = 64, .height = 64, .extraUsage = rhi::TextureUsage::TransferSrc});
        fill.colorAttachment(color, LoadOp::Clear, {1.0F, 0.0F, 0.0F, 1.0F});
        fill.depthAttachment(
            fill.createTexture("Depth", {.format = rhi::Format::D32Float, .width = 64, .height = 64}),
            LoadOp::Clear, 0.0F, rhi::StoreOp::DontCare);
        auto window2 = graph.addPass({.name = "Window", .canDisable = false, .record = kNothing});
        window2.sampleTexture(color);
        window2.windowAttachment(LoadOp::Clear);
        REQUIRE(graph.compile().hasValue());
        rhi::CommandList commands;
        for (int i = 0; i < 8; ++i) {
            commands.reset();
            REQUIRE(graph.execute(commands).hasValue());
            device.beginFrame();
            device.submit(commands);
            device.present();
        }
        std::vector<std::byte> pixels(std::size_t{64} * 64 * 4);
        REQUIRE(device.readTexture(graph.texture(color), 0, 0, pixels).hasValue());
        CHECK(pixels[0] == std::byte{255});
        CHECK(pixels[1] == std::byte{0});
        CHECK(graph.passGpuMilliseconds(0).has_value());
        CHECK(graph.passGpuMilliseconds(1).has_value());
        CHECK(device.stats().apiErrors == 0);
        CHECK(device.stats().invalidCommands == 0);
    }
    device.waitIdle();
    CHECK(device.liveResources().empty());
}
