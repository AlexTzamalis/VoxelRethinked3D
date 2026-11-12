#include "vxe/render/entity_renderer.hpp"

#include <vxe/core/profile.hpp>

#include <algorithm>
#include <cstring>

namespace vxe::render {

Result<std::unique_ptr<EntityRenderer>> EntityRenderer::create(const EntityRendererDesc& desc) {
    rhi::Device& device = *desc.device;
    std::unique_ptr<EntityRenderer> entities(new EntityRenderer(device));
    EntityRenderer& e = *entities;
    const auto fail = [](const Error& error) { return error.withContext("Entities"); };
    const auto fragment = rhi::createShaderFromFile(device, "render/entity.frag", desc.shaders);
    if (!fragment) {
        return fail(fragment.error());
    }
    for (const bool hand : {false, true}) {
        std::vector<rhi::ShaderDefine> defines;
        if (hand) {
            defines.push_back({.name = "VXE_ENTITY_FIRST_PERSON"});
        }
        const auto vertex = rhi::createShaderFromFile(device, "render/entity.vert", desc.shaders, defines);
        if (!vertex) {
            device.destroy(fragment.value());
            return fail(vertex.error());
        }
        rhi::PipelineDesc pipeline{.vertex = vertex.value(),
                                   .fragment = fragment.value(),
                                   .debugName = hand ? "Entities (first person)" : "Entities"};
        // Boxes are closed, but the second skin layer is seen from inside
        // through its holes; both sides it is.
        pipeline.raster.cullMode = rhi::CullMode::None;
        pipeline.depth.testEnabled = true;
        pipeline.depth.writeEnabled = true;
        pipeline.depth.compare = rhi::CompareOp::GreaterOrEqual; // Reversed Z (ADR-010).
        pipeline.depthFormat = desc.depthFormat;
        pipeline.colorFormats[0] = desc.colorFormat;
        pipeline.colorCount = 1;
        auto created = device.createPipeline(pipeline);
        device.destroy(vertex.value());
        if (!created) {
            device.destroy(fragment.value());
            return fail(created.error());
        }
        (hand ? e.m_firstPerson : e.m_world) = created.value();
    }
    device.destroy(fragment.value());
    auto sampler = device.createSampler({.minFilter = rhi::Filter::Nearest,
                                         .magFilter = rhi::Filter::Nearest,
                                         .mipmapMode = rhi::MipmapMode::None,
                                         .addressU = rhi::AddressMode::ClampToEdge,
                                         .addressV = rhi::AddressMode::ClampToEdge,
                                         .debugName = "Entity textures"});
    if (!sampler) {
        return fail(sampler.error());
    }
    e.m_sampler = sampler.value();
    auto textures =
        device.createTexture({.type = rhi::TextureType::Texture2DArray,
                              .format = rhi::Format::RGBA8Srgb,
                              .width = kEntityTextureSize,
                              .height = kEntityTextureSize,
                              .layers = kEntityTextureLayers,
                              .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst,
                              .debugName = "Entity textures"});
    if (!textures) {
        return fail(textures.error());
    }
    e.m_textures = textures.value();
    const std::vector<u32> grey(usize{kEntityTextureSize} * kEntityTextureSize, 0xFF808080U);
    for (u32 layer = 0; layer < kEntityTextureLayers; ++layer) {
        e.uploadLayer(layer, grey);
    }
    VXE_TRY(e.m_ring.create(device, usize{kEntityMaxVertices} * sizeof(EntityVertex),
                            rhi::BufferUsage::Storage, "Entity vertices"));
    return entities;
}

EntityRenderer::~EntityRenderer() {
    m_device.destroy(m_world);
    m_device.destroy(m_firstPerson);
    m_device.destroy(m_sampler);
    m_device.destroy(m_textures);
    m_ring.destroy(m_device);
}

// Pixels come top row first; textures start at the bottom row (ADR-016), and
// the shader turns v around to match.
void EntityRenderer::uploadLayer(u32 layer, std::span<const u32> pixels) {
    constexpr usize kRow = kEntityTextureSize;
    std::vector<u32> flipped(kRow * kRow);
    for (usize y = 0; y < kRow; ++y) {
        std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>((kRow - 1 - y) * kRow), kRow,
                    flipped.begin() + static_cast<std::ptrdiff_t>(y * kRow));
    }
    static_cast<void>(m_device.uploadTexture(
        m_textures, {.layer = layer, .data = std::as_bytes(std::span<const u32>(flipped))}));
}

void EntityRenderer::setSkin(const PackImage* image) {
    std::vector<u32> pixels(usize{kEntityTextureSize} * kEntityTextureSize, 0xFF808080U);
    if (image != nullptr && image->width >= kEntityTextureSize && image->height >= kEntityTextureSize / 2) {
        // Old skins are 64 x 32: their lower half stays as it is (no left limbs
        // of their own); bigger skins are read at their top left 64 x 64.
        const u32 rows = std::min(image->height, kEntityTextureSize);
        for (u32 y = 0; y < rows; ++y) {
            for (u32 x = 0; x < kEntityTextureSize; ++x) {
                pixels[(usize{y} * kEntityTextureSize) + x] = image->pixels[(usize{y} * image->width) + x];
            }
        }
    }
    uploadLayer(kEntitySkinLayer, pixels);
}

void EntityRenderer::setItemFaces(std::span<const std::vector<u32>, 6> faces) {
    constexpr u32 kFace = 16;
    constexpr u32 kScale = kEntityTextureSize / kFace;
    std::vector<u32> pixels(usize{kEntityTextureSize} * kEntityTextureSize);
    for (u32 face = 0; face < 6; ++face) {
        const std::vector<u32>& source = faces[face];
        for (u32 y = 0; y < kEntityTextureSize; ++y) {
            for (u32 x = 0; x < kEntityTextureSize; ++x) {
                const usize from = (usize{y / kScale} * kFace) + (x / kScale);
                pixels[(usize{y} * kEntityTextureSize) + x] = from < source.size() ? source[from] : 0U;
            }
        }
        uploadLayer(kEntityItemLayer + face, pixels);
    }
}

void EntityRenderer::beginFrame() {
    for (std::vector<EntityVertex>& vertices : m_vertices) {
        vertices.clear();
    }
}

void EntityRenderer::add(EntityLayer layer, std::span<const EntityVertex> vertices) {
    std::vector<EntityVertex>& list = m_vertices.at(static_cast<usize>(layer));
    const usize room =
        kEntityMaxVertices - std::min<usize>(kEntityMaxVertices, m_vertices[0].size() + m_vertices[1].size());
    const usize count = std::min(vertices.size(), room) / 3 * 3;
    list.insert(list.end(), vertices.begin(), vertices.begin() + static_cast<std::ptrdiff_t>(count));
}

void EntityRenderer::upload() {
    VXE_PROFILE_FUNCTION();
    const std::span<std::byte> memory = m_ring.next(m_device);
    usize offset = 0;
    for (const std::vector<EntityVertex>& vertices : m_vertices) {
        const usize bytes = vertices.size() * sizeof(EntityVertex);
        if (bytes != 0) {
            std::memcpy(memory.data() + offset, vertices.data(), bytes);
        }
        offset += bytes;
    }
}

void EntityRenderer::record(rhi::CommandList& commands, EntityLayer layer) const {
    const usize index = static_cast<usize>(layer);
    const std::vector<EntityVertex>& vertices = m_vertices.at(index);
    if (vertices.empty()) {
        return;
    }
    const usize first = index == 0 ? 0 : m_vertices[0].size();
    commands.bindPipeline(layer == EntityLayer::World ? m_world : m_firstPerson);
    commands.bindStorageBuffer(2, 0, m_ring.current(), first * sizeof(EntityVertex),
                               vertices.size() * sizeof(EntityVertex));
    commands.bindTexture(2, 1, m_textures, m_sampler);
    commands.draw(static_cast<u32>(vertices.size()));
}

} // namespace vxe::render
