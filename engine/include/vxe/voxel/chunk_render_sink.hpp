#pragma once

#include <vxe/render/chunk_renderer.hpp>
#include <vxe/voxel/mesher.hpp>
#include <vxe/voxel/streaming.hpp>

namespace vxe::voxel {

// The chunk renderer's view of a mesh: the same quads and groups as plain
// data (render sits below voxel in the layers, so it cannot take a
// ChunkMesh itself). The geometry points into `mesh`.
[[nodiscard]] render::ChunkGeometry toChunkGeometry(const ChunkMesh& mesh) noexcept;

// Hands the streaming manager's meshes to a ChunkRenderer, the usual sink in
// a game client.
//
// Thread safety: main thread only, like the renderer.
class ChunkRenderSink final : public ChunkMeshSink {
public:
    explicit ChunkRenderSink(render::ChunkRenderer& renderer) noexcept
        : m_renderer(renderer) {}

    [[nodiscard]] Result upload(const ChunkMesh& mesh) override;
    void remove(ChunkPos position) override;
    void setBudgetScale(f32 scale) override;

private:
    render::ChunkRenderer& m_renderer;
};

} // namespace vxe::voxel
