#include "vxe/voxel/chunk_render_sink.hpp"

#include <span>

namespace vxe::voxel {

static_assert(render::kChunkFaceGroups == kFaceGroupCount, "The renderer and the mesher agree on the groups");
static_assert(render::kChunkQuadBytes == sizeof(PackedQuad), "The renderer and the mesher agree on quads");

render::ChunkGeometry toChunkGeometry(const ChunkMesh& mesh) noexcept {
    return {.position = mesh.position,
            .quads = std::as_bytes(std::span(mesh.quads)),
            .groupBegin = mesh.groupBegin,
            .connectivity = mesh.connectivity,
            .bounds = {.min = mesh.boundsMin, .max = mesh.boundsMax}};
}

ChunkMeshSink::Result ChunkRenderSink::upload(const ChunkMesh& mesh) {
    switch (m_renderer.upload(toChunkGeometry(mesh))) {
    case render::ChunkRenderer::UploadResult::Uploaded:
        return Result::Accepted;
    case render::ChunkRenderer::UploadResult::OverBudget:
        return Result::OverBudget;
    case render::ChunkRenderer::UploadResult::OutOfMemory:
        break;
    }
    return Result::Refused;
}

void ChunkRenderSink::remove(ChunkPos position) {
    static_cast<void>(m_renderer.remove(position));
}

void ChunkRenderSink::setBudgetScale(f32 scale) {
    m_renderer.setUploadBudgetScale(scale);
}

} // namespace vxe::voxel
