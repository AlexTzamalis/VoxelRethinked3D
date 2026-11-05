#include "vxe/render/renderer.hpp"

#include <vxe/core/profile.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace vxe::render {
namespace {

// The scene is lit in linear HDR; the post pass tonemaps it for the window.
constexpr rhi::Format kSceneFormat = rhi::Format::RGBA16Float;

// How strong sky light is for the chunks: full while the sun is up, fading
// through dusk to moonlight (a quarter) when it is down. With the sky off it
// is always day.
f32 daylightOf(const SkyParams& sky) noexcept {
    if (sky.flags.x == 0) {
        return 1.0F;
    }
    const f32 t = std::clamp((sky.sunDirection.y + 0.1F) / 0.35F, 0.0F, 1.0F);
    const f32 smooth = t * t * (3.0F - (2.0F * t));
    return 0.25F + (0.75F * smooth);
}
constexpr rhi::Format kDepthFormat = rhi::Format::D32Float;

// Lines showing the view of a camera out to `distance` blocks, for
// Debug.FreezeCulling: what is outside them was culled by the frustum.
void drawFrustum(DebugDraw& lines, const Camera& camera, f32 aspect, f64 distance) {
    constexpr u32 kColor = debugColor(255, 220, 40);
    const f64 halfHeight = std::tan(static_cast<f64>(glm::radians(camera.fovYDegrees)) * 0.5);
    const DVec3 forward(camera.forward());
    const DVec3 right(camera.right());
    const DVec3 up(camera.up());
    std::array<DVec3, 4> corners{};
    for (usize i = 0; i < corners.size(); ++i) {
        const f64 sx = (i == 1 || i == 2) ? 1.0 : -1.0;
        const f64 sy = i >= 2 ? 1.0 : -1.0;
        const DVec3 direction =
            forward + (right * (sx * halfHeight * static_cast<f64>(aspect))) + (up * (sy * halfHeight));
        corners.at(i) = camera.position + (direction * distance);
    }
    for (usize i = 0; i < corners.size(); ++i) {
        lines.line(camera.position, corners.at(i), kColor);
        lines.line(corners.at(i), corners.at((i + 1) % corners.size()), kColor);
    }
}

} // namespace

Result<std::unique_ptr<Renderer>> Renderer::create(const RendererDesc& desc) {
    VXE_ASSERT(desc.device != nullptr && desc.settings != nullptr, "Renderer needs a device and settings");
    std::unique_ptr<Renderer> renderer(new Renderer(*desc.device));
    renderer->m_shaders = desc.shaders;
    rhi::Device& device = *desc.device;
    const auto fail = [](const Error& error) { return error.withContext("Renderer"); };

    if (Result<void> created = renderer->m_constants.create(device); !created) {
        return fail(created.error());
    }
    auto chunks = ChunkRenderer::create({.device = &device,
                                         .settings = desc.settings,
                                         .stats = desc.stats,
                                         .shaders = desc.shaders,
                                         .colorFormat = kSceneFormat,
                                         .depthFormat = kDepthFormat,
                                         .arenaBytes = desc.chunkArenaBytes,
                                         .maxChunks = desc.maxChunks});
    if (!chunks) {
        return fail(chunks.error());
    }
    renderer->m_chunks = std::move(chunks.value());
    auto lod = LodRenderer::create({.device = &device,
                                    .settings = desc.settings,
                                    .stats = desc.stats,
                                    .shaders = desc.shaders,
                                    .colorFormat = kSceneFormat,
                                    .depthFormat = kDepthFormat,
                                    .arenaBytes = desc.lodArenaBytes});
    if (!lod) {
        return fail(lod.error());
    }
    renderer->m_lod = std::move(lod.value());
    auto sky = SkyRenderer::create({.device = &device,
                                    .settings = desc.settings,
                                    .stats = desc.stats,
                                    .shaders = desc.shaders,
                                    .colorFormat = kSceneFormat});
    if (!sky) {
        return fail(sky.error());
    }
    renderer->m_sky = std::move(sky.value());
    auto precipitation = PrecipitationRenderer::create({.device = &device,
                                                        .settings = desc.settings,
                                                        .shaders = desc.shaders,
                                                        .colorFormat = kSceneFormat,
                                                        .depthFormat = kDepthFormat});
    if (!precipitation) {
        return fail(precipitation.error());
    }
    renderer->m_precipitation = std::move(precipitation.value());
    auto clouds = CloudRenderer::create({.device = &device,
                                         .settings = desc.settings,
                                         .stats = desc.stats,
                                         .shaders = desc.shaders,
                                         .colorFormat = kSceneFormat});
    if (!clouds) {
        return fail(clouds.error());
    }
    renderer->m_clouds = std::move(clouds.value());
    auto godRays = GodRayRenderer::create(
        {.device = &device, .settings = desc.settings, .shaders = desc.shaders, .colorFormat = kSceneFormat});
    if (!godRays) {
        return fail(godRays.error());
    }
    renderer->m_godRays = std::move(godRays.value());
    auto post = PostProcess::create({.device = &device, .settings = desc.settings, .shaders = desc.shaders});
    if (!post) {
        return fail(post.error());
    }
    renderer->m_post = std::move(post.value());
    auto lines = DebugDraw::create({.device = &device,
                                    .settings = desc.settings,
                                    .stats = desc.stats,
                                    .input = desc.input,
                                    .toggleContext = desc.debugContext,
                                    .shaders = desc.shaders,
                                    .colorFormat = kSceneFormat,
                                    .depthFormat = kDepthFormat});
    if (!lines) {
        return fail(lines.error());
    }
    renderer->m_debugDraw = std::move(lines.value());
    if (device.capabilities().timestampQueries) {
        renderer->m_timerCreated = renderer->m_timer.create(device).hasValue();
    }
    renderer->m_graph = std::make_unique<FrameGraph>(
        FrameGraphDesc{.device = &device, .settings = desc.settings, .stats = desc.stats});
    if (Result<void> built = renderer->buildGraph(); !built) {
        return fail(built.error());
    }
    return renderer;
}

Renderer::~Renderer() {
    // The graph's transient targets and the pieces' buffers go first; the
    // device itself belongs to the owner.
    m_graph.reset();
    m_debugDraw.reset();
    m_post.reset();
    m_godRays.reset();
    m_clouds.reset();
    m_precipitation.reset();
    m_sky.reset();
    m_lod.reset();
    m_chunks.reset();
    m_constants.destroy(m_device);
    if (m_timerCreated) {
        m_timer.destroy(m_device);
    }
}

Result<void> Renderer::buildGraph() {
    FrameGraph& graph = *m_graph;
    static_cast<void>(graph.addPass(
        {.name = "ChunkUpload", .type = PassType::Transfer, .record = [this](PassContext& context) {
             m_chunks->recordUploads(context.commands);
         }}));
    static_cast<void>(graph.addPass(
        {.name = "LodUpload", .type = PassType::Transfer, .record = [this](PassContext& context) {
             m_lod->recordUploads(context.commands);
         }}));
    // Switched off, every chunk face group is drawn.
    static_cast<void>(graph.addPass(
        {.name = "Visibility", .type = PassType::Compute, .record = [this](PassContext& context) {
             m_chunks->recordCulling(context.commands);
         }}));

    // Sun and moon shadows: terrain from the light, before the chunks read it.
    auto shadows = graph.addPass({.name = "Shadows", .record = [this](PassContext& context) {
                                      m_constants.bind(context.commands);
                                      m_chunks->recordShadows(context.commands);
                                  }});
    const TextureRef shadowMap = shadows.createTexture(
        "ShadowMap",
        {.format = rhi::Format::D32Float, .width = kShadowAtlasSize, .height = kShadowAtlasSize});
    shadows.depthAttachment(shadowMap, rhi::LoadOp::Clear, 0.0F);

    auto chunks = graph.addPass({.name = "Chunks", .record = [this, shadowMap](PassContext& context) {
                                     m_constants.bind(context.commands);
                                     m_chunks->setShadowMap(context.texture(shadowMap));
                                     m_chunks->record(context.commands, ChunkPass::Opaque);
                                     m_chunks->record(context.commands, ChunkPass::Cutout);
                                     m_chunks->record(context.commands, ChunkPass::Plants);
                                 }});
    const TextureRef color = chunks.createTexture("SceneColor", {.format = kSceneFormat});
    const TextureRef depth = chunks.createTexture("SceneDepth", {.format = kDepthFormat});
    // Reversed Z: depth 0 is infinitely far (ADR-010).
    chunks.colorAttachment(color, rhi::LoadOp::Clear, {0.0F, 0.0F, 0.0F, 1.0F});
    chunks.depthAttachment(depth, rhi::LoadOp::Clear, 0.0F);
    chunks.sampleTexture(shadowMap);

    // Far terrain after the chunks: its depth test against them is cheap,
    // and the coverage map keeps it off columns they draw.
    auto lod = graph.addPass({.name = "Lod", .record = [this](PassContext& context) {
                                  m_constants.bind(context.commands);
                                  m_lod->record(context.commands);
                              }});
    lod.colorAttachment(color, rhi::LoadOp::Load);
    lod.depthAttachment(depth, rhi::LoadOp::Load);

    auto sky = graph.addPass({.name = "Sky", .record = [this, depth](PassContext& context) {
                                  m_constants.bind(context.commands);
                                  m_sky->record(context.commands, context.texture(depth));
                              }});
    sky.colorAttachment(color, rhi::LoadOp::Load);
    sky.sampleTexture(depth);

    auto clouds = graph.addPass({.name = "Clouds", .record = [this, depth](PassContext& context) {
                                     m_constants.bind(context.commands);
                                     m_clouds->recordLayer(context.commands, context.texture(depth), *m_sky);
                                 }});
    const TextureRef cloudLayer = clouds.createTexture("CloudLayer", {.format = kSceneFormat, .scale = 0.5F});
    clouds.colorAttachment(cloudLayer, rhi::LoadOp::Clear, {0.0F, 0.0F, 0.0F, 0.0F});
    clouds.sampleTexture(depth);

    auto cloudBlend =
        graph.addPass({.name = "CloudsBlend", .record = [this, cloudLayer](PassContext& context) {
                           m_clouds->recordBlend(context.commands, context.texture(cloudLayer));
                       }});
    cloudBlend.colorAttachment(color, rhi::LoadOp::Load);
    cloudBlend.sampleTexture(cloudLayer);

    // Sun rays: shafts through the gaps between what stands before the sun.
    auto godRays = graph.addPass(
        {.name = "GodRays", .record = [this, depth, cloudLayer](PassContext& context) {
             m_godRays->recordLayer(context.commands, context.texture(depth), context.texture(cloudLayer));
         }});
    const TextureRef rayLayer = godRays.createTexture("RayLayer", {.format = kSceneFormat, .scale = 0.5F});
    godRays.colorAttachment(rayLayer, rhi::LoadOp::Clear, {0.0F, 0.0F, 0.0F, 0.0F});
    godRays.sampleTexture(depth);
    godRays.sampleTexture(cloudLayer);
    auto godRayBlend =
        graph.addPass({.name = "GodRaysBlend", .record = [this, rayLayer](PassContext& context) {
                           m_godRays->recordBlend(context.commands, context.texture(rayLayer));
                       }});
    godRayBlend.colorAttachment(color, rhi::LoadOp::Load);
    godRayBlend.sampleTexture(rayLayer);

    auto translucent =
        graph.addPass({.name = "ChunksTranslucent", .record = [this, shadowMap](PassContext& context) {
                           m_constants.bind(context.commands);
                           m_chunks->setShadowMap(context.texture(shadowMap));
                           m_sky->bindParams(context.commands, 1);
                           m_chunks->record(context.commands, ChunkPass::Translucent);
                       }});
    translucent.colorAttachment(color, rhi::LoadOp::Load);
    translucent.depthAttachment(depth, rhi::LoadOp::Load);
    translucent.sampleTexture(shadowMap);

    // Rain and snow last, in front of water, glass and the clouds.
    auto precipitation = graph.addPass({.name = "Precipitation", .record = [this](PassContext& context) {
                                            m_constants.bind(context.commands);
                                            m_precipitation->record(context.commands);
                                        }});
    precipitation.colorAttachment(color, rhi::LoadOp::Load);
    precipitation.depthAttachment(depth, rhi::LoadOp::Load);

    // The depth pyramid for next frame's occlusion culling. Translucent
    // chunks write no depth, so it holds what the opaque and cutout ones hid.
    auto hiZ = graph.addPass(
        {.name = "HiZ", .type = PassType::Compute, .record = [this, depth](PassContext& context) {
             m_chunks->recordOcclusionDepth(context.commands, context.texture(depth));
         }});
    hiZ.sampleTexture(depth);

    auto lines = graph.addPass({.name = "DebugDraw", .record = [this](PassContext& context) {
                                    m_constants.bind(context.commands);
                                    m_debugDraw->record(context.commands);
                                }});
    lines.colorAttachment(color, rhi::LoadOp::Load);
    lines.depthAttachment(depth, rhi::LoadOp::Load, 0.0F, rhi::StoreOp::DontCare);

    auto post = graph.addPass({.name = "Post", .record = [this, color](PassContext& context) {
                                   m_post->record(context.commands, context.texture(color));
                               }});
    post.sampleTexture(color);
    post.windowAttachment(rhi::LoadOp::DontCare);

    graph
        .addPass({.name = "Ui",
                  .record =
                      [this](PassContext& context) {
                          if (m_ui) {
                              m_ui(context.commands);
                          }
                      }})
        .windowAttachment(rhi::LoadOp::Load);

    // Not switchable: the debug UI is how passes are switched back on.
    graph
        .addPass({.name = "WindowOverlay",
                  .canDisable = false,
                  .record =
                      [this](PassContext& context) {
                          if (m_windowOverlay) {
                              m_windowOverlay(context.commands);
                          }
                      }})
        .windowAttachment(rhi::LoadOp::Load);
    return graph.compile();
}

void Renderer::setWindowOverlay(std::function<void(rhi::CommandList&)> record) {
    m_windowOverlay = std::move(record);
}

void Renderer::setUi(std::function<void(rhi::CommandList&)> record) {
    m_ui = std::move(record);
}

void Renderer::beginFrame(const Camera& camera, const FrameTime& time) {
    VXE_PROFILE_FUNCTION();
    m_camera = camera;
    const u32 width = std::max(m_device.backBufferWidth(), 1U);
    const u32 height = std::max(m_device.backBufferHeight(), 1U);
    m_graph->setOutputSize(width, height);
    m_sky->update(time.deltaSeconds);
    m_clouds->update(time.deltaSeconds, m_sky->atmosphere().cloudCover, m_sky->timeSpeed());
    const f32 daylight = daylightOf(m_sky->params());
    const Vec3 skyLight(m_sky->params().skyLight);
    m_chunks->setDaylight(daylight);
    m_chunks->setSkyLight(skyLight);
    m_chunks->setShadowLight(
        shadowLightOf(Vec3(m_sky->params().sunDirection), Vec3(m_sky->params().moonDirection)));
    m_precipitation->update(camera, time.elapsedSeconds, skyLight);
    m_chunks->setWetness(m_precipitation->wetness());
    m_lod->setLight(skyLight, m_chunks->brightness(), m_chunks->faceShading(), m_precipitation->wetness());
    m_post->update(width, height);
    m_constants.update(m_device, makeFrameConstants(camera, {.width = width,
                                                             .height = height,
                                                             .seconds = time.elapsedSeconds,
                                                             .deltaSeconds = time.deltaSeconds,
                                                             .frameIndex = m_frameIndex}));
    m_chunks->setViewSize(width, height);
    m_chunks->beginFrame(camera);
    m_godRays->update(camera, m_chunks->viewAspect(), Vec3(m_sky->params().sunDirection),
                      Vec3(m_sky->params().sunColor));
    m_lod->setViewSize(width, height);
    m_lod->beginFrame(camera);
    m_debugDraw->handleInput();
    m_debugDraw->beginFrame(camera);
    m_lod->drawDebug(*m_debugDraw);
    if (const std::optional<Camera> frozen = m_chunks->frozenCullingCamera()) {
        drawFrustum(*m_debugDraw, *frozen, m_chunks->viewAspect(), 96.0);
    }
    m_frameBegun = true;
}

Result<void> Renderer::endFrame(const FrameTime& time) {
    VXE_PROFILE_FUNCTION();
    if (!m_frameBegun) {
        beginFrame(m_camera, time);
    }
    m_frameBegun = false;
    ++m_frameIndex;
    m_commands.reset();
    if (m_timerCreated) {
        m_timer.beginFrame(m_device, m_commands);
    }
    const Result<void> executed = m_graph->execute(m_commands);
    if (m_timerCreated) {
        m_timer.endFrame(m_commands);
    }
    m_device.beginFrame();
    m_device.submit(m_commands);
    m_device.present();
    return executed;
}

} // namespace vxe::render
