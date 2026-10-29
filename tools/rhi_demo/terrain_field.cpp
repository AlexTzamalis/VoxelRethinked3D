#include "terrain_field.hpp"

#include "voxel/terrain_sample.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/time.hpp>
#include <vxe/voxel/block_registry.hpp>
#include <vxe/voxel/voxel_world.hpp>

#include <array>

namespace vxe::demo {
namespace {

using voxel::BlockRenderInfo;
using voxel::BlockStateId;
using voxel::RenderLayer;

constexpr i32 kFieldWidth = 32;  // Chunks along X and Z.
constexpr i32 kFieldBottom = -2; // Chunk Y range: blocks -32 to 95, around the sample's surface.
constexpr i32 kFieldTop = 5;

// Placeholder texture layers (RGBA8, red in the low byte) and which layer
// each face of the sample's states uses.
constexpr u16 kStoneLayer = 0;
constexpr u16 kDirtLayer = 1;
constexpr u16 kGrassTopLayer = 2;
constexpr u16 kGrassSideLayer = 3;
constexpr u16 kCoalLayer = 4;
constexpr u16 kIronLayer = 5;
constexpr u16 kGoldLayer = 6;
constexpr u16 kWaterLayer = 7;
constexpr u16 kSandLayer = 8;
constexpr u16 kGravelLayer = 9;
constexpr usize kLayerCount = 10;

constexpr u32 rgba(u32 red, u32 green, u32 blue, u32 alpha = 255) noexcept {
    return red | (green << 8U) | (blue << 16U) | (alpha << 24U);
}

constexpr std::array<u32, kLayerCount> kColors = {
    rgba(125, 125, 128), rgba(121, 85, 58),  rgba(95, 159, 53),      rgba(110, 120, 60),  rgba(60, 60, 64),
    rgba(170, 140, 115), rgba(220, 190, 60), rgba(38, 82, 170, 200), rgba(220, 205, 150), rgba(130, 122, 118),
};

std::shared_ptr<voxel::BlockRegistryView> sampleBlocks() {
    voxel::BlockRegistryBuilder builder;
    const auto add = [&](RenderLayer layer, u16 texture, u16 top, u16 bottom) {
        BlockRenderInfo info{.layer = layer};
        info.faceTextureLayer.fill(texture);
        info.faceTextureLayer[static_cast<usize>(voxel::BlockFace::PosY)] = top;
        info.faceTextureLayer[static_cast<usize>(voxel::BlockFace::NegY)] = bottom;
        static_cast<void>(builder.add(info));
    };
    // In the order of test::terrain: stone, dirt, grass, coal, iron, gold,
    // water, sand, gravel.
    add(RenderLayer::Opaque, kStoneLayer, kStoneLayer, kStoneLayer);
    add(RenderLayer::Opaque, kDirtLayer, kDirtLayer, kDirtLayer);
    add(RenderLayer::Opaque, kGrassSideLayer, kGrassTopLayer, kDirtLayer);
    add(RenderLayer::Opaque, kCoalLayer, kCoalLayer, kCoalLayer);
    add(RenderLayer::Opaque, kIronLayer, kIronLayer, kIronLayer);
    add(RenderLayer::Opaque, kGoldLayer, kGoldLayer, kGoldLayer);
    add(RenderLayer::Translucent, kWaterLayer, kWaterLayer, kWaterLayer);
    add(RenderLayer::Opaque, kSandLayer, kSandLayer, kSandLayer);
    add(RenderLayer::Opaque, kGravelLayer, kGravelLayer, kGravelLayer);
    return builder.build();
}

} // namespace

Result<std::unique_ptr<TerrainField>> TerrainField::create(const TerrainFieldDesc& desc) {
    std::unique_ptr<TerrainField> field(new TerrainField(*desc.device));
    auto renderer = render::ChunkRenderer::create({.device = desc.device,
                                                   .settings = desc.settings,
                                                   .stats = desc.stats,
                                                   .shaders = desc.shaders,
                                                   .colorFormat = desc.colorFormat,
                                                   .arenaBytes = usize{64} << 20U});
    if (!renderer) {
        return renderer.error();
    }
    field->m_renderer = std::move(renderer.value());
    auto textures = render::createPlaceholderBlockTextures(*desc.device, kColors);
    if (!textures) {
        return textures.error();
    }
    field->m_textures = textures.value();
    field->m_renderer->setTextures(field->m_textures.texture, field->m_textures.sampler);

    // Generate the whole field, then mesh every chunk from a snapshot so
    // borders see their real neighbors.
    const Stopwatch stopwatch;
    voxel::VoxelWorld world;
    std::array<BlockStateId, kChunkVolume> states{};
    const i32 half = kFieldWidth / 2;
    for (i32 cz = desc.center.z - half; cz < desc.center.z + half; ++cz) {
        for (i32 cx = desc.center.x - half; cx < desc.center.x + half; ++cx) {
            for (i32 cy = kFieldBottom; cy <= kFieldTop; ++cy) {
                const ChunkPos position{.x = cx, .y = cy, .z = cz};
                test::sampleChunk(position, states);
                voxel::ChunkStorage storage;
                storage.assign(states);
                world.insertChunk(position, std::move(storage));
            }
        }
    }
    const f64 generated = stopwatch.elapsedMilliseconds();
    const auto blocks = sampleBlocks();
    voxel::Mesher mesher;
    auto snapshot = std::make_unique<voxel::ChunkSnapshot>();
    voxel::ChunkMesh mesh;
    world.forEachChunk([&](const voxel::Chunk& chunk) {
        static_cast<void>(world.createSnapshot(chunk.position(), *snapshot));
        mesher.mesh(*snapshot, *blocks, {}, mesh);
        if (!mesh.empty()) {
            field->m_meshes.push_back(mesh);
        }
    });
    VXE_LOG_INFO(log::core, "Terrain field: {} chunks generated in {:.0f} ms, {} meshes in {:.0f} ms",
                 world.chunkCount(), generated, field->m_meshes.size(),
                 stopwatch.elapsedMilliseconds() - generated);
    return field;
}

TerrainField::~TerrainField() {
    m_renderer.reset();
    if (m_textures.texture.isValid()) {
        m_device.destroy(m_textures.texture);
    }
    if (m_textures.sampler.isValid()) {
        m_device.destroy(m_textures.sampler);
    }
}

void TerrainField::update(const render::Camera& camera) {
    m_renderer->beginFrame(camera);
    while (m_uploaded < m_meshes.size()) {
        const voxel::ChunkMesh& mesh = m_meshes[m_uploaded];
        const render::ChunkGeometry geometry{.position = mesh.position,
                                             .quads = std::as_bytes(std::span(mesh.quads)),
                                             .groupBegin = mesh.groupBegin};
        if (m_renderer->upload(geometry) != render::ChunkRenderer::UploadResult::Uploaded) {
            break; // Budget used up: the rest goes in later frames.
        }
        ++m_uploaded;
    }
    ++m_uploadFrames;
    if (m_uploaded == m_meshes.size() && !m_meshes.empty()) {
        const render::ChunkRenderer& renderer = *m_renderer;
        VXE_LOG_INFO(log::core,
                     "Terrain field on the GPU after {} frames: {} chunks, {} quads, {:.0f} bytes per chunk "
                     "(quads and metadata), arena {:.1f} MiB",
                     m_uploadFrames, renderer.residentChunks(), renderer.residentQuads(),
                     static_cast<f64>(renderer.residentQuads() * render::kChunkQuadBytes) /
                             static_cast<f64>(renderer.residentChunks()) +
                         16.0,
                     static_cast<f64>(renderer.arenaUsedBytes()) / (1024.0 * 1024.0));
        m_meshes.clear(); // All on the GPU; the CPU copies are no longer needed.
        m_meshes.shrink_to_fit();
        m_uploaded = 0;
    }
}

} // namespace vxe::demo
