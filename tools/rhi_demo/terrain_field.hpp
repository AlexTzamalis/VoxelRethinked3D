#pragma once

// A field of terrain chunks for vxe_rhi_demo --terrain (the E9.5 acceptance
// scene): 32 x 8 x 32 chunks of the synthetic terrain sample, meshed with
// the binary greedy mesher and drawn by the chunk renderer.

#include <vxe/config/settings.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/chunk_renderer.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>
#include <vxe/voxel/mesher.hpp>

#include <memory>
#include <vector>

namespace vxe::demo {

struct TerrainFieldDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr;
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA16Float;
    ChunkPos center{}; // The field spans 32 x 32 chunk columns around this chunk.
};

class TerrainField {
public:
    // Generates and meshes every chunk on the calling thread (about a
    // second); uploads then go out over the next frames within the budget.
    [[nodiscard]] static Result<std::unique_ptr<TerrainField>> create(const TerrainFieldDesc& desc);
    ~TerrainField();
    TerrainField(const TerrainField&) = delete;
    TerrainField& operator=(const TerrainField&) = delete;
    TerrainField(TerrainField&&) = delete;
    TerrainField& operator=(TerrainField&&) = delete;

    // Starts the renderer's frame and uploads what the budget allows.
    void update(const render::Camera& camera);
    [[nodiscard]] render::ChunkRenderer& renderer() noexcept { return *m_renderer; }
    [[nodiscard]] usize pendingUploads() const noexcept { return m_meshes.size() - m_uploaded; }

private:
    explicit TerrainField(rhi::Device& device) noexcept
        : m_device(device) {}

    rhi::Device& m_device;
    std::unique_ptr<render::ChunkRenderer> m_renderer;
    render::BlockTextures m_textures{};
    std::vector<voxel::ChunkMesh> m_meshes;
    usize m_uploaded = 0;
    u64 m_uploadFrames = 0;
};

} // namespace vxe::demo
