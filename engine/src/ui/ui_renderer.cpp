#include "vxe/ui/ui_renderer.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/profile.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <thread>

namespace vxe::ui {
namespace {

// Must match the UiProjection block in ui/quad.vert.
struct Projection {
    f32 scale[2];
    f32 translate[2];
};

// The atlas starts this wide and doubles until the images fit.
constexpr u32 kAtlasStartWidth = 256;
constexpr u32 kAtlasMaxWidth = 4096;

// Shelf packing at a given width: images sorted by height go left to right,
// a new shelf starts when a row is full. Returns false when an image is
// wider than the atlas.
bool tryPack(std::span<const AtlasImage> images, std::span<const usize> order, u32 width, AtlasPixels& out) {
    u32 x = 0;
    u32 y = 0;
    u32 shelf = 0;
    out.regions.assign(images.size(), {});
    std::vector<std::pair<u32, u32>> positions(images.size());
    for (const usize index : order) {
        const AtlasImage& image = images[index];
        if (image.width + 1 > width) {
            return false;
        }
        if (x + image.width + 1 > width) {
            x = 0;
            y += shelf + 1;
            shelf = 0;
        }
        positions[index] = {x, y};
        x += image.width + 1;
        shelf = std::max(shelf, image.height);
    }
    const u32 height = std::bit_ceil(std::max(1U, y + shelf + 1));
    out.width = width;
    out.height = height;
    out.pixels.assign(usize{width} * height, 0U);
    for (usize index = 0; index < images.size(); ++index) {
        const AtlasImage& image = images[index];
        const auto [left, top] = positions[index];
        for (u32 row = 0; row < image.height; ++row) {
            std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(usize{row} * image.width),
                        image.width,
                        out.pixels.begin() + static_cast<std::ptrdiff_t>(usize{top + row} * width + left));
        }
        const auto w = static_cast<f32>(width);
        const auto h = static_cast<f32>(height);
        out.regions[index] = {image.name,
                              {.u0 = static_cast<f32>(left) / w,
                               .v0 = static_cast<f32>(top) / h,
                               .u1 = static_cast<f32>(left + image.width) / w,
                               .v1 = static_cast<f32>(top + image.height) / h,
                               .width = image.width,
                               .height = image.height}};
    }
    return true;
}

} // namespace

AtlasPixels packAtlas(std::span<const AtlasImage> images) {
    std::vector<usize> order(images.size());
    for (usize i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [&](usize a, usize b) { return images[a].height > images[b].height; });
    AtlasPixels atlas;
    for (u32 width = kAtlasStartWidth; width <= kAtlasMaxWidth; width *= 2) {
        // A square atlas is the goal: grow the width while the shelves
        // stack higher than wide.
        if (tryPack(images, order, width, atlas) && atlas.height <= width) {
            return atlas;
        }
    }
    static_cast<void>(tryPack(images, order, kAtlasMaxWidth, atlas));
    return atlas;
}

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
    // The panorama wraps around horizontally and is smooth, unlike pixel art.
    auto panoramaSampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                                 .magFilter = rhi::Filter::Linear,
                                                 .mipmapMode = rhi::MipmapMode::None,
                                                 .addressU = rhi::AddressMode::Repeat,
                                                 .addressV = rhi::AddressMode::ClampToEdge,
                                                 .debugName = "UI panorama sampler"});
    if (!panoramaSampler) {
        return panoramaSampler.error().withContext("UI renderer");
    }
    renderer->m_panoramaSampler = panoramaSampler.value();
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
        renderer->m_atlasBytesStat = gauge("Ui.AtlasBytes", "GPU memory of the UI atlas (font and images).");
    }
    return renderer;
}

UiRenderer::~UiRenderer() {
    for (const FrameBuffers& set : m_frames) {
        m_device.destroy(set.quads);
        m_device.destroy(set.uniforms);
        m_device.destroy(set.fence);
    }
    m_device.destroy(m_atlas);
    m_device.destroy(m_panorama);
    m_device.destroy(m_pipeline);
    m_device.destroy(m_sampler);
    m_device.destroy(m_panoramaSampler);
}

Result<void> UiRenderer::setResources(const AtlasImage& fontSheet, std::span<const AtlasImage> images) {
    auto font = PixelFont::fromSheet(fontSheet.pixels, fontSheet.width, fontSheet.height);
    if (!font) {
        return font.error();
    }
    std::vector<AtlasImage> all;
    all.reserve(images.size() + 1);
    all.push_back(
        {.name = {}, .width = fontSheet.width, .height = fontSheet.height, .pixels = fontSheet.pixels});
    for (const AtlasImage& image : images) {
        if (image.width == 0 || image.height == 0 ||
            image.pixels.size() != usize{image.width} * image.height) {
            return Error(ErrorCode::InvalidArgument,
                         std::format("UI image '{}' has {} pixels for {} x {}", image.name,
                                     image.pixels.size(), image.width, image.height));
        }
        all.push_back(image);
    }
    AtlasPixels atlas = packAtlas(all);
    auto texture =
        m_device.createTexture({.format = rhi::Format::RGBA8Unorm,
                                .width = atlas.width,
                                .height = atlas.height,
                                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                .debugName = "UI atlas"});
    if (!texture) {
        return texture.error();
    }
    // Uploaded top row first (not ADR-016's bottom row first), so a v
    // coordinate counts from the atlas's top, like the regions.
    if (auto uploaded = m_device.uploadTexture(
            texture.value(), {.mipLevel = 0, .layer = 0, .data = std::as_bytes(std::span(atlas.pixels))});
        !uploaded) {
        m_device.destroy(texture.value());
        return uploaded.error();
    }
    m_device.destroy(m_atlas); // Deferred until the GPU is done with it (E6.6).
    m_atlas = texture.value();
    m_resources.font = std::move(font).value();
    m_resources.fontRegion = atlas.regions.front().second;
    m_resources.images.assign(atlas.regions.begin() + 1, atlas.regions.end());
    ++m_resources.version;
    if (m_stats != nullptr && m_atlasBytesStat.isValid()) {
        m_stats->set(m_atlasBytesStat, static_cast<f64>(atlas.pixels.size() * sizeof(u32)));
    }
    return {};
}

Result<void> UiRenderer::setFont(std::span<const u32> pixels, u32 width, u32 height) {
    return setResources({.name = {}, .width = width, .height = height, .pixels = pixels}, {});
}

Result<void> UiRenderer::setPanorama(std::span<const u32> pixels, u32 width, u32 height) {
    if (width == 0 || height == 0 || pixels.size() != usize{width} * height) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("Panorama has {} pixels for {} x {}", pixels.size(), width, height));
    }
    auto texture =
        m_device.createTexture({.format = rhi::Format::RGBA8Unorm,
                                .width = width,
                                .height = height,
                                .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                .debugName = "UI panorama"});
    if (!texture) {
        return texture.error();
    }
    // Top row first like the atlas, so v counts down from the top.
    if (auto uploaded = m_device.uploadTexture(texture.value(),
                                               {.mipLevel = 0, .layer = 0, .data = std::as_bytes(pixels)});
        !uploaded) {
        m_device.destroy(texture.value());
        return uploaded.error();
    }
    m_device.destroy(m_panorama); // Deferred until the GPU is done with it (E6.6).
    m_panorama = texture.value();
    return {};
}

void UiRenderer::drawPanorama(f32 yaw, f32 pitch, f32 fovYDegrees) {
    if (!m_panorama.isValid()) {
        return;
    }
    const auto width = static_cast<f32>(m_width);
    const auto height = static_cast<f32>(m_height);
    const f32 tanHalfY = std::tan(fovYDegrees * 0.5F * std::numbers::pi_v<f32> / 180.0F);
    m_frame.panorama({.x = 0.0F, .y = 0.0F, .width = width, .height = height}, yaw, pitch,
                     tanHalfY * width / height, tanHalfY);
}

void UiRenderer::begin(u32 width, u32 height) {
    m_width = std::max(width, 1U);
    m_height = std::max(height, 1U);
    m_frame.reset(&m_resources);
}

TextSize UiRenderer::measureText(std::string_view text, u32 scale) const noexcept {
    if (!m_resources.font) {
        return {};
    }
    const TextSize size = m_resources.font->measure(text);
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
    const usize quadCount = m_frame.size();
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
        FrameBuffers& previous = m_frames[m_frameIndex];
        m_device.destroy(previous.fence);
        previous.fence = m_device.insertFence();
        m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
    }
    m_recordedOnce = true;
    FrameBuffers& set = m_frames[m_frameIndex];
    const usize bytes = quadCount * sizeof(UiQuad);
    if (!prepareSet(set, bytes)) {
        return;
    }
    std::memcpy(m_device.mappedRange(set.quads).data(), m_frame.quads().data(), bytes);
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
    if (m_atlas.isValid()) {
        commands.bindTexture(2, 1, m_atlas, m_sampler);
    }
    if (m_panorama.isValid()) {
        commands.bindTexture(2, 2, m_panorama, m_panoramaSampler);
    }
    commands.draw(static_cast<u32>(quadCount * 6));
    commands.endDebugGroup();
}

} // namespace vxe::ui
