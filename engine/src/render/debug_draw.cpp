#include "vxe/render/debug_draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vxe::render {
namespace {

constexpr u16 kKeyF3 = 60;
constexpr u16 kKeyG = 10;

constexpr u32 kCameraChunkColor = debugColor(255, 220, 60);
constexpr u32 kChunkCornerColor = debugColor(80, 150, 255, 200);

} // namespace

DebugDraw::DebugDraw(const DebugDrawDesc& desc)
    : m_device(*desc.device)
    , m_stats(desc.stats)
    , m_input(desc.input)
    , m_maxLines(std::max(desc.maxLines, 1U)) {
    m_vertices.reserve(static_cast<usize>(m_maxLines) * 2);
    using config::Visibility;
    m_chunkBorders = desc.settings->registerBool({{.id = "Debug.Draw.ChunkBorders",
                                                   .displayName = "Chunk borders",
                                                   .description = "Outlines the camera's chunk and marks the "
                                                                  "corners of the chunks around it (F3+G).",
                                                   .visibility = Visibility::Advanced},
                                                  false});
    m_chunkBorderRadius = desc.settings->registerInt({{.id = "Debug.Draw.ChunkBorderRadius",
                                                       .displayName = "Chunk border radius",
                                                       .description = "Chunks around the camera that get "
                                                                      "corner lines.",
                                                       .visibility = Visibility::Advanced},
                                                      2,
                                                      0,
                                                      8,
                                                      1});
    m_depthTest = desc.settings->registerBool({{.id = "Debug.Draw.DepthTest",
                                                .displayName = "Debug lines hidden by terrain",
                                                .description = "Off draws debug lines through everything.",
                                                .visibility = Visibility::Developer},
                                               true});
    if (m_input != nullptr && desc.toggleContext.isValid()) {
        m_toggleChunkBorders = m_input->registerAction({.id = "debug.draw.chunk_borders",
                                                        .displayName = "Toggle chunk borders",
                                                        .category = "Debug",
                                                        .context = desc.toggleContext,
                                                        .defaults = {input::Binding::chord(kKeyF3, kKeyG)}});
    }
    if (m_stats != nullptr) {
        const auto stat = [this](const stats::StatDesc& statDesc) {
            auto id = m_stats->registerStat(statDesc);
            return id ? id.value() : stats::StatId{};
        };
        m_linesStat =
            stat({.name = "Render.DebugDraw.Lines", .description = "Debug lines drawn this frame."});
        m_droppedStat =
            stat({.name = "Render.DebugDraw.Dropped",
                  .description = "Debug lines dropped this frame because the line limit was reached."});
    }
}

Result<std::unique_ptr<DebugDraw>> DebugDraw::create(const DebugDrawDesc& desc) {
    std::unique_ptr<DebugDraw> draw(new DebugDraw(desc));
    rhi::Device& device = *desc.device;
    const auto vertex = rhi::createShaderFromFile(device, "debug/lines.vert", desc.shaders);
    if (!vertex) {
        return vertex.error().withContext("Debug draw");
    }
    const auto fragment = rhi::createShaderFromFile(device, "debug/lines.frag", desc.shaders);
    if (!fragment) {
        device.destroy(vertex.value());
        return fragment.error().withContext("Debug draw");
    }
    rhi::PipelineDesc pipeline{.vertex = vertex.value(),
                               .fragment = fragment.value(),
                               .topology = rhi::PrimitiveTopology::Lines,
                               .debugName = "Debug lines"};
    pipeline.raster.cullMode = rhi::CullMode::None;
    pipeline.depth.writeEnabled = false; // Lines never hide each other or the scene.
    pipeline.blend[0] = rhi::BlendState::alphaBlend();
    pipeline.colorFormats[0] = desc.colorFormat;
    pipeline.colorCount = 1;
    pipeline.depthFormat = desc.depthFormat;
    auto depthTested = device.createPipeline(pipeline);
    pipeline.depth.testEnabled = false;
    pipeline.debugName = "Debug lines on top";
    auto onTop = device.createPipeline(pipeline);
    device.destroy(vertex.value());
    device.destroy(fragment.value());
    if (!depthTested || !onTop) {
        if (depthTested) {
            device.destroy(depthTested.value());
        }
        if (onTop) {
            device.destroy(onTop.value());
        }
        return (depthTested ? onTop.error() : depthTested.error()).withContext("Debug draw");
    }
    draw->m_depthTested = depthTested.value();
    draw->m_onTop = onTop.value();
    VXE_TRY(draw->m_ring.create(device, static_cast<usize>(draw->m_maxLines) * 2 * sizeof(LineVertex),
                                rhi::BufferUsage::Storage, "Debug lines"));
    return draw;
}

DebugDraw::~DebugDraw() {
    m_device.destroy(m_depthTested);
    m_device.destroy(m_onTop);
    m_ring.destroy(m_device);
}

void DebugDraw::handleInput() {
    if (m_input != nullptr && m_toggleChunkBorders.isValid() && m_input->isPressed(m_toggleChunkBorders)) {
        static_cast<void>(m_chunkBorders.set(!m_chunkBorders.get()));
    }
}

void DebugDraw::beginFrame(const Camera& camera) {
    m_vertices.clear();
    m_droppedThisFrame = 0;
    m_origin = camera.position;
    m_cameraChunk = camera.renderPosition().chunk;
    if (m_chunkBorders.get()) {
        addChunkBorders();
    }
}

void DebugDraw::line(const DVec3& from, const DVec3& to, u32 color) {
    if (m_vertices.size() + 2 > m_vertices.capacity()) {
        ++m_droppedThisFrame;
        ++m_dropped;
        return;
    }
    // Camera relative in double first, then float: exact near the camera
    // wherever the camera is.
    m_vertices.push_back({.position = Vec3(from - m_origin), .color = color});
    m_vertices.push_back({.position = Vec3(to - m_origin), .color = color});
}

void DebugDraw::box(const DVec3& min, const DVec3& max, u32 color) {
    const auto corner = [&min, &max](int bits) {
        return DVec3((bits & 1) != 0 ? max.x : min.x, (bits & 2) != 0 ? max.y : min.y,
                     (bits & 4) != 0 ? max.z : min.z);
    };
    // The 12 edges join corners that differ in exactly one coordinate bit.
    for (int a = 0; a < 8; ++a) {
        for (int bit = 1; bit < 8; bit <<= 1) {
            if ((a & bit) == 0) {
                line(corner(a), corner(a | bit), color);
            }
        }
    }
}

void DebugDraw::chunkBox(const ChunkPos& chunk, u32 color) {
    const BlockPos origin = chunkOrigin(chunk);
    const DVec3 min(static_cast<f64>(origin.x), static_cast<f64>(origin.y), static_cast<f64>(origin.z));
    box(min, min + DVec3(kChunkSize), color);
}

void DebugDraw::grid(const DVec3& center, f64 y, i32 halfCount, f64 spacing, u32 color, u32 majorColor,
                     i32 majorEvery) {
    // Snapped to the spacing in double, so the lines sit on the same world
    // coordinates while the camera moves.
    const f64 baseX = std::floor(center.x / spacing);
    const f64 baseZ = std::floor(center.z / spacing);
    const f64 extent = static_cast<f64>(halfCount) * spacing;
    for (i32 i = -halfCount; i <= halfCount; ++i) {
        const f64 x = (baseX + i) * spacing;
        const f64 z = (baseZ + i) * spacing;
        const auto isMajor = [majorEvery](f64 index) {
            return majorEvery > 0 && std::fmod(std::abs(index), static_cast<f64>(majorEvery)) == 0.0;
        };
        const f64 startX = baseX * spacing - extent;
        const f64 startZ = baseZ * spacing - extent;
        line({x, y, startZ}, {x, y, startZ + 2.0 * extent}, isMajor(baseX + i) ? majorColor : color);
        line({startX, y, z}, {startX + 2.0 * extent, y, z}, isMajor(baseZ + i) ? majorColor : color);
    }
}

void DebugDraw::addChunkBorders() {
    chunkBox(m_cameraChunk, kCameraChunkColor);
    const i32 radius = m_chunkBorderRadius.get();
    const BlockPos origin = chunkOrigin(m_cameraChunk);
    // Vertical lines at the corners of the chunk columns around the camera,
    // from one chunk below to two above the camera's chunk.
    const f64 bottom = static_cast<f64>(origin.y - kChunkSize);
    const f64 top = static_cast<f64>(origin.y + (2 * static_cast<i64>(kChunkSize)));
    for (i32 dz = -radius; dz <= radius + 1; ++dz) {
        for (i32 dx = -radius; dx <= radius + 1; ++dx) {
            const f64 x = static_cast<f64>(origin.x + static_cast<i64>(dx) * kChunkSize);
            const f64 z = static_cast<f64>(origin.z + static_cast<i64>(dz) * kChunkSize);
            line({x, bottom, z}, {x, top, z}, kChunkCornerColor);
        }
    }
}

void DebugDraw::record(rhi::CommandList& commands) {
    if (m_stats != nullptr) {
        m_stats->add(m_linesStat, static_cast<i64>(lineCount()));
        m_stats->add(m_droppedStat, static_cast<i64>(m_droppedThisFrame));
    }
    if (m_vertices.empty()) {
        return;
    }
    const usize bytes = m_vertices.size() * sizeof(LineVertex);
    const std::span<std::byte> memory = m_ring.next(m_device);
    std::memcpy(memory.data(), m_vertices.data(), bytes);
    commands.bindPipeline(m_depthTest.get() ? m_depthTested : m_onTop);
    commands.bindStorageBuffer(2, 0, m_ring.current(), 0, bytes);
    commands.draw(static_cast<u32>(m_vertices.size()));
}

} // namespace vxe::render
