#include "vxe/render/precipitation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe::render {
namespace {

// The layout must match PrecipitationParams in precipitation.vert.
struct PrecipitationParams {
    Vec4 shape;          // Amount, kind (0 rain, 1 snow), seconds, top relative to the camera's chunk.
    Vec4 light;          // The color of sky light.
    glm::ivec4 corner{}; // x and z: the grid's corner, relative to the camera's chunk.
};

constexpr usize kColumns = static_cast<usize>(kPrecipitationSide) * kPrecipitationSide;
constexpr u32 kVerticesPerColumn = 6;
constexpr f64 kWrapSeconds = 3600.0; // Keeps the scrolling exact in floats.
constexpr f64 kWettingSeconds = 30.0;
constexpr f64 kDryingSeconds = 240.0;

} // namespace

PrecipitationRenderer::PrecipitationRenderer(const PrecipitationDesc& desc)
    : m_device(*desc.device)
    , m_tops(kColumns, kOpenColumn)
    , m_relativeTops(kColumns, 0.0F) {
    m_enabled = desc.settings->registerBool(
        {{.id = "Graphics.Sky.Precipitation",
          .displayName = "Rain and snow",
          .description = "Draws falling rain and snow when the weather brings them."},
         true});
}

Result<std::unique_ptr<PrecipitationRenderer>> PrecipitationRenderer::create(const PrecipitationDesc& desc) {
    std::unique_ptr<PrecipitationRenderer> rain(new PrecipitationRenderer(desc));
    rhi::Device& device = *desc.device;
    const auto vertex = rhi::createShaderFromFile(device, "render/precipitation.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Precipitation");
    }
    const auto fragment = rhi::createShaderFromFile(device, "render/precipitation.frag", desc.shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("Precipitation");
    }
    rhi::PipelineDesc pipeline{
        .vertex = vertex.value(), .fragment = fragment.value(), .debugName = "Precipitation"};
    pipeline.raster.cullMode = rhi::CullMode::None;
    // Behind terrain the streaks are hidden, and they hide nothing themselves.
    pipeline.depth.testEnabled = true;
    pipeline.depth.writeEnabled = false;
    pipeline.depth.compare = rhi::CompareOp::GreaterOrEqual;
    pipeline.depthFormat = desc.depthFormat;
    pipeline.blend[0] = rhi::BlendState::alphaBlend();
    pipeline.colorFormats[0] = desc.colorFormat;
    pipeline.colorCount = 1;
    auto created = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    if (!created) {
        return created.error().withContext("Precipitation");
    }
    rain->m_pipeline = created.value();
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::Repeat,
                                         .addressV = rhi::AddressMode::Repeat,
                                         .debugName = "Precipitation sampler"});
    if (!sampler) {
        return sampler.error().withContext("Precipitation");
    }
    rain->m_sampler = sampler.value();
    auto streaks = device.createTexture({.type = rhi::TextureType::Texture2DArray,
                                         .format = rhi::Format::RGBA8Srgb,
                                         .width = kPrecipitationImageWidth,
                                         .height = kPrecipitationImageHeight,
                                         .layers = kPrecipitationImages.size(),
                                         .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                         .debugName = "Rain and snow"});
    if (!streaks) {
        return streaks.error().withContext("Precipitation");
    }
    rain->m_streaks = streaks.value();
    auto tops = device.createTexture({.format = rhi::Format::R32Float,
                                      .width = kPrecipitationSide,
                                      .height = kPrecipitationSide,
                                      .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                                      .debugName = "Precipitation column tops"});
    if (!tops) {
        return tops.error().withContext("Precipitation");
    }
    rain->m_topsTexture = tops.value();
    VXE_TRY(rain->m_ring.create(device, sizeof(PrecipitationParams), rhi::BufferUsage::Uniform,
                                "Precipitation parameters"));
    return rain;
}

PrecipitationRenderer::~PrecipitationRenderer() {
    m_device.destroy(m_pipeline);
    m_device.destroy(m_sampler);
    m_device.destroy(m_streaks);
    m_device.destroy(m_topsTexture);
    m_ring.destroy(m_device);
}

void PrecipitationRenderer::setImages(std::span<const PackImage* const> images) {
    std::vector<u32> scaled(usize{kPrecipitationImageWidth} * kPrecipitationImageHeight);
    for (u32 layer = 0; layer < kPrecipitationImages.size(); ++layer) {
        const PackImage* image = layer < images.size() ? images[layer] : nullptr;
        m_loaded.at(layer) = image != nullptr && image->width != 0 && image->height != 0;
        if (!m_loaded.at(layer)) {
            continue;
        }
        for (u32 y = 0; y < kPrecipitationImageHeight; ++y) {
            for (u32 x = 0; x < kPrecipitationImageWidth; ++x) {
                const u32 fromX = (x * image->width) / kPrecipitationImageWidth;
                const u32 fromY = (y * image->height) / kPrecipitationImageHeight;
                scaled[(usize{y} * kPrecipitationImageWidth) + x] =
                    image->pixels[(usize{fromY} * image->width) + fromX];
            }
        }
        static_cast<void>(m_device.uploadTexture(
            m_streaks, {.layer = layer, .data = std::as_bytes(std::span<const u32>(scaled))}));
    }
}

void PrecipitationRenderer::setWeather(PrecipitationKind kind, f32 amount) noexcept {
    m_kind = kind;
    m_amount = std::clamp(amount, 0.0F, 1.0F);
}

void PrecipitationRenderer::setColumns(i64 cornerX, i64 cornerZ, std::span<const i32> tops) {
    m_cornerX = cornerX;
    m_cornerZ = cornerZ;
    const usize count = std::min(tops.size(), m_tops.size());
    std::copy_n(tops.begin(), count, m_tops.begin());
    std::fill(m_tops.begin() + static_cast<std::ptrdiff_t>(count), m_tops.end(), kOpenColumn);
}

bool PrecipitationRenderer::isFalling() const noexcept {
    return m_enabled.get() && m_kind != PrecipitationKind::None && m_amount > 0.0F &&
           m_loaded.at(static_cast<usize>(m_kind) - 1);
}

void PrecipitationRenderer::update(const Camera& camera, f64 seconds, const Vec3& skyLight, f64 fallsFromY) {
    const f64 step = m_lastSeconds < 0.0 ? 0.0 : std::clamp(seconds - m_lastSeconds, 0.0, 1.0);
    m_lastSeconds = seconds;
    const bool raining = m_enabled.get() && m_kind == PrecipitationKind::Rain;
    const f32 soaked = raining ? m_amount : 0.0F;
    const f64 seconds_to = soaked > m_wetness ? kWettingSeconds : kDryingSeconds;
    m_wetness += static_cast<f32>(std::min(step / seconds_to, 1.0)) * (soaked - m_wetness);
    if (!isFalling()) {
        return;
    }
    // Everything relative to the camera's chunk, whole numbers in integer math,
    // so the columns sit exactly on the blocks at any distance from the origin.
    const CameraPosition position = camera.renderPosition();
    const i64 chunkX = i64{position.chunk.x} * kChunkSize;
    const i64 chunkY = i64{position.chunk.y} * kChunkSize;
    const i64 chunkZ = i64{position.chunk.z} * kChunkSize;
    for (usize i = 0; i < kColumns; ++i) {
        m_relativeTops[i] = m_tops[i] == kOpenColumn ? -1.0e6F : static_cast<f32>(i64{m_tops[i]} - chunkY);
    }
    static_cast<void>(
        m_device.uploadTexture(m_topsTexture, {.data = std::as_bytes(std::span<const f32>(m_relativeTops))}));
    const PrecipitationParams params{.shape = Vec4(m_amount, m_kind == PrecipitationKind::Snow ? 1.0F : 0.0F,
                                                   static_cast<f32>(std::fmod(seconds, kWrapSeconds)),
                                                   static_cast<f32>(fallsFromY - static_cast<f64>(chunkY))),
                                     .light = Vec4(skyLight, 1.0F),
                                     .corner = glm::ivec4(static_cast<i32>(m_cornerX - chunkX), 0,
                                                          static_cast<i32>(m_cornerZ - chunkZ), 0)};
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), &params, sizeof(params));
}

void PrecipitationRenderer::record(rhi::CommandList& commands) const {
    if (!isFalling()) {
        return;
    }
    commands.bindPipeline(m_pipeline);
    commands.bindUniformBuffer(1, 0, m_ring.current(), 0, sizeof(PrecipitationParams));
    commands.bindTexture(2, 0, m_topsTexture, m_sampler);
    commands.bindTexture(2, 1, m_streaks, m_sampler);
    // A streak and a splash per column; the shader drops what does not show.
    commands.draw(static_cast<u32>(kColumns) * kVerticesPerColumn * 2U);
}

} // namespace vxe::render
