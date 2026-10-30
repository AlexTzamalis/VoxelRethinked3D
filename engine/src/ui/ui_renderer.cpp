#include "vxe/ui/ui_renderer.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <thread>

namespace vxe::ui {
namespace {

constexpr u32 kFlagTextured = 1;

// Must match the UiProjection block in ui/quad.vert.
struct Projection {
    f32 scale[2];
    f32 translate[2];
};

// The drop shadow color: the text color at a quarter brightness, same alpha.
constexpr u32 shadowOf(u32 color) noexcept {
    return ((color & 0x00FCFCFCU) >> 2U) | (color & 0xFF000000U);
}

} // namespace

Result<std::unique_ptr<UiRenderer>> UiRenderer::create(const UiRendererDesc& desc) {
    rhi::Device& device = *desc.device;
    std::unique_ptr<UiRenderer> renderer(new UiRenderer(device));
    const auto vertex = rhi::createShaderFromFile(device, "ui/quad.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("UI renderer");
    }
    const auto fragment = rhi::createShaderFromFile(device, "ui/quad.frag", desc.shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("UI renderer");
    }
    rhi::PipelineDesc pipelineDesc{.vertex = vertex.value(), .fragment = fragment.value(), .debugName = "UI"};
    pipelineDesc.raster.cullMode = rhi::CullMode::None;
    pipelineDesc.depth.testEnabled = false;
    pipelineDesc.depth.writeEnabled = false;
    pipelineDesc.blend[0] = rhi::BlendState::alphaBlend();
    pipelineDesc.colorFormats[0] = desc.targetFormat;
    pipelineDesc.colorCount = 1;
    auto pipeline = device.createPipeline(pipelineDesc);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    if (!pipeline) {
        return pipeline.error().withContext("UI renderer");
    }
    renderer->m_pipeline = pipeline.value();
    // Nearest filtering keeps every font pixel a sharp square.
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "UI sampler"});
    if (!sampler) {
        return sampler.error().withContext("UI renderer");
    }
    renderer->m_sampler = sampler.value();
    for (FrameBuffers& set : renderer->m_frames) {
        auto uniforms = device.createBuffer({.sizeBytes = 256,
                                             .usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::CpuWrite,
                                             .debugName = "UI projection"});
        if (!uniforms) {
            return uniforms.error().withContext("UI renderer");
        }
        set.uniforms = uniforms.value();
    }
    if (desc.stats != nullptr) {
        renderer->m_stats = desc.stats;
        const auto gauge = [&](std::string_view name, std::string_view description) {
            auto id = desc.stats->registerStat(
                {.name = name, .description = description, .type = stats::StatType::Gauge});
            return id ? id.value() : stats::StatId{};
        };
        renderer->m_quadsStat = gauge("Ui.Quads", "Rectangles and glyphs the game UI drew last frame.");
        renderer->m_drawCallsStat = gauge("Ui.DrawCalls", "Draw calls of the game UI last frame.");
    }
    return renderer;
}

UiRenderer::~UiRenderer() {
    for (const FrameBuffers& set : m_frames) {
        m_device.destroy(set.quads);
        m_device.destroy(set.uniforms);
        m_device.destroy(set.fence);
    }
    m_device.destroy(m_fontTexture);
    m_device.destroy(m_pipeline);
    m_device.destroy(m_sampler);
}

Result<void> UiRenderer::setFont(std::span<const u32> pixels, u32 width, u32 height) {
    auto font = PixelFont::fromSheet(pixels, width, height);
    if (!font) {
        return font.error();
    }
    auto texture =
        m_device.createTexture({.format = rhi::Format::RGBA8Unorm,
                                .width = width,
                                .height = height,
                                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                .debugName = "UI font"});
    if (!texture) {
        return texture.error();
    }
    // Uploaded top row first (not ADR-016's bottom row first), so a v
    // coordinate counts from the sheet's top, as PixelFont computes them.
    if (auto uploaded = m_device.uploadTexture(texture.value(),
                                               {.mipLevel = 0, .layer = 0, .data = std::as_bytes(pixels)});
        !uploaded) {
        m_device.destroy(texture.value());
        return uploaded.error();
    }
    m_device.destroy(m_fontTexture); // Deferred until the GPU is done with it (E6.6).
    m_fontTexture = texture.value();
    m_font = std::move(font).value();
    ++m_fontVersion;
    return {};
}

void UiRenderer::begin(u32 width, u32 height) {
    m_width = std::max(width, 1U);
    m_height = std::max(height, 1U);
    m_quads.clear();
}

void UiRenderer::fillRect(const Rect& rect, u32 color) {
    m_quads.push_back(
        {.rect = {std::round(rect.x), std::round(rect.y), std::round(rect.width), std::round(rect.height)},
         .color = color});
}

void UiRenderer::addGlyphs(const PixelFont& font, std::string_view text, f32 x, f32 y, u32 scale, u32 color) {
    m_glyphs.clear();
    font.layout(text, m_glyphs);
    const auto size = static_cast<f32>(scale);
    for (const GlyphQuad& glyph : m_glyphs) {
        m_quads.push_back(
            {.rect = {x + glyph.x * size, y + glyph.y * size, glyph.width * size, glyph.height * size},
             .uv = {glyph.glyph->u0, glyph.glyph->v0, glyph.glyph->u1, glyph.glyph->v1},
             .color = color,
             .flags = kFlagTextured});
    }
}

void UiRenderer::drawText(std::string_view text, f32 x, f32 y, const TextStyle& style) {
    if (!m_font || text.empty()) {
        return;
    }
    const u32 scale = std::max(style.scale, 1U);
    const f32 left = std::round(x);
    const f32 top = std::round(y);
    if (style.shadow) {
        addGlyphs(*m_font, text, left + static_cast<f32>(scale), top + static_cast<f32>(scale), scale,
                  shadowOf(style.color));
    }
    addGlyphs(*m_font, text, left, top, scale, style.color);
}

TextSize UiRenderer::measureText(std::string_view text, u32 scale) const noexcept {
    if (!m_font) {
        return {};
    }
    const TextSize size = m_font->measure(text);
    const auto factor = static_cast<f32>(std::max(scale, 1U));
    return {.width = size.width * factor, .height = size.height * factor};
}

// Makes the set's buffer big enough and makes sure the GPU no longer reads
// it. With three sets the GPU would have to be three frames behind to wait.
bool UiRenderer::prepareSet(FrameBuffers& set, usize bytes) {
    if (set.fence.isValid()) {
        while (!m_device.isFenceSignaled(set.fence)) {
            std::this_thread::yield();
        }
        m_device.destroy(set.fence);
        set.fence = {};
    }
    if (set.capacity >= bytes && set.quads.isValid()) {
        return true;
    }
    // Powers of two, so a growing UI does not make a new buffer every frame.
    const usize size = std::max<usize>(std::bit_ceil(bytes), usize{48} * 1024);
    auto created = m_device.createBuffer({.sizeBytes = size,
                                          .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CpuWrite,
                                          .debugName = "UI quads"});
    if (!created) {
        VXE_LOG_ERROR(log::rhi, "UI renderer: {}", created.error().describe());
        return false;
    }
    m_device.destroy(set.quads);
    set.quads = created.value();
    set.capacity = size;
    return true;
}

void UiRenderer::record(rhi::CommandList& commands) {
    VXE_PROFILE_FUNCTION();
    const usize quadCount = m_quads.size();
    if (m_stats != nullptr) {
        if (m_quadsStat.isValid()) {
            m_stats->set(m_quadsStat, static_cast<f64>(quadCount));
        }
        if (m_drawCallsStat.isValid()) {
            m_stats->set(m_drawCallsStat, quadCount > 0 ? 1.0 : 0.0);
        }
    }
    if (quadCount == 0) {
        return;
    }
    // The previous frame's set gets its fence now, after that frame was
    // submitted, so the fence signals once the GPU has finished it.
    if (m_recordedOnce) {
        FrameBuffers& previous = m_frames[m_frame];
        m_device.destroy(previous.fence);
        previous.fence = m_device.insertFence();
        m_frame = (m_frame + 1) % kFramesInFlight;
    }
    m_recordedOnce = true;
    FrameBuffers& set = m_frames[m_frame];
    const usize bytes = quadCount * sizeof(Quad);
    if (!prepareSet(set, bytes)) {
        return;
    }
    std::memcpy(m_device.mappedRange(set.quads).data(), m_quads.data(), bytes);
    // Window pixels (origin top left, y down) to clip space (y up).
    const auto width = static_cast<f32>(m_width);
    const auto height = static_cast<f32>(m_height);
    const Projection projection{.scale = {2.0F / width, -2.0F / height}, .translate = {-1.0F, 1.0F}};
    std::memcpy(m_device.mappedRange(set.uniforms).data(), &projection, sizeof(projection));

    commands.beginDebugGroup("UI");
    commands.setViewport(0.0F, 0.0F, width, height);
    commands.bindPipeline(m_pipeline);
    commands.bindUniformBuffer(1, 0, set.uniforms, 0, sizeof(Projection));
    commands.bindStorageBuffer(2, 0, set.quads, 0, bytes);
    if (m_fontTexture.isValid()) {
        commands.bindTexture(2, 1, m_fontTexture, m_sampler);
    }
    commands.draw(static_cast<u32>(quadCount * 6));
    commands.endDebugGroup();
}

} // namespace vxe::ui
