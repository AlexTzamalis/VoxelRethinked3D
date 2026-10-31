#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/containers.hpp>
#include <vxe/core/offset_allocator.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/cpu_write_ring.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/rhi/command_list.hpp>
#include <vxe/rhi/device.hpp>
#include <vxe/rhi/shader_source.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace vxe::render {

// Bytes of one far terrain quad (voxel::LodQuad).
inline constexpr usize kLodQuadBytes = 12;
// Chunk columns along each side of the coverage map. Columns wrap around it,
// so any render distance below half of it works (Section 13.5).
inline constexpr i32 kLodCoverageSize = 128;
// Colors in the LOD material table (voxel::kLodMaxMaterial + 1).
inline constexpr usize kLodMaterialCount = 4096;

// One tile's mesh as the renderer takes it (render sits below voxel, so
// this is plain data, like ChunkGeometry).
struct LodGeometry {
    u64 id = 0;      // Any value unique per tile (the voxel side packs the tile key).
    i64 originX = 0; // The tile's -X -Z corner, in blocks.
    i64 originZ = 0;
    u32 cellSize = 1; // Blocks per cell.
    u32 level = 1;    // For debug colors.
    i32 baseY = 0;    // Quad heights count from here.
    i32 minY = 0;     // The box the quads stay in, for culling.
    i32 maxY = 0;
    std::span<const std::byte> quads{}; // 12 bytes per quad.
};

struct LodRendererDesc {
    rhi::Device* device = nullptr;
    config::SettingsRegistry* settings = nullptr;
    stats::StatRegistry* stats = nullptr; // Optional.
    rhi::ShaderFileReader shaders{};
    rhi::Format colorFormat = rhi::Format::RGBA16Float;
    rhi::Format depthFormat = rhi::Format::D32Float;
    usize arenaBytes = usize{96} << 20U; // Quads of every shown tile.
    u32 maxTiles = 2048;
    usize stagingBytesPerFrame = usize{8} << 20U;
};

// Draws far terrain tiles (Section 13.5). Like the chunk renderer, every
// tile's quads live in one GPU buffer (an arena with an OffsetAllocator)
// and the vertex shader builds each quad's corners from its 12 bytes
// (render/lod.vert). There are few tiles (hundreds), so culling is a
// frustum test per tile on the CPU, and the per tile data (position relative
// to the camera) is written fresh every frame; that also keeps positions
// exact at any distance from the origin (ADR-011).
//
// Coverage map (Section 13.5): an 8 bit texture marks the chunk columns
// where real chunks are drawn; the LOD shader discards its fragments there,
// so the far terrain never shows through or over real terrain, and stays
// where chunks are not ready yet (no holes). Columns wrap around the map
// (kLodCoverageSize), so moving needs no update, only changes do.
//
// Colors come from a material table (setMaterialColors): the average color
// of each block texture (G2.3), indexed by the quads' material.
//
// Each frame: beginFrame(camera), any number of upload() and remove(),
// recordUploads() in a transfer pass, record() in the scene pass with the
// frame constants bound.
//
// Tiles whose every chunk column is covered are not drawn at all (most of
// the finest level, under the real chunks), which saves their vertex work.
//
// Settings: Graphics.LOD.CoverageDiscard (developer), Debug.LOD.ColorLevels,
// Debug.LOD.ShowCoverage (covered columns in magenta instead of hidden),
// Debug.LOD.TileBorders (each tile's box, colored by level).
// Stats: Render.Lod.Tiles, .Quads, .ArenaBytes, .DrawnTiles, .DrawnQuads, .CoveredTiles, .UploadBytes,
// .UploadsOverBudget and .UploadsOutOfMemory.
//
// Thread safety: main thread only, like the device.
class LodRenderer {
public:
    enum class UploadResult : u8 {
        Uploaded,
        OverBudget,
        OutOfMemory
    };

    [[nodiscard]] static Result<std::unique_ptr<LodRenderer>> create(const LodRendererDesc& desc);
    ~LodRenderer();
    LodRenderer(const LodRenderer&) = delete;
    LodRenderer& operator=(const LodRenderer&) = delete;
    LodRenderer(LodRenderer&&) = delete;
    LodRenderer& operator=(LodRenderer&&) = delete;

    void beginFrame(const Camera& camera);
    // Adds or replaces the tile with geometry.id. OverBudget: the upload
    // budget of this frame is used, try again next frame.
    [[nodiscard]] UploadResult upload(const LodGeometry& geometry);
    bool remove(u64 id);
    // Removes every tile and clears the coverage map (leaving a world).
    void clear();
    [[nodiscard]] bool isResident(u64 id) const noexcept { return m_tiles.contains(id); }
    [[nodiscard]] usize tileCount() const noexcept { return m_tiles.size(); }

    // RGBA8 colors (0xAABBGGRR, as unpackUnorm4x8 reads them), at most
    // kLodMaterialCount; missing entries stay magenta.
    void setMaterialColors(std::span<const u32> colors);
    // Marks a chunk column as drawn by real chunks (or not any more).
    void setCovered(i32 chunkX, i32 chunkZ, bool covered) noexcept;
    // Real chunks stand in this column up to the top of chunk layer `topChunkY`,
    // and far terrain continues above that: a mountain taller than the loaded
    // layers keeps its far top over its real foot, with neither drawn twice.
    void setCoveredBelow(i32 chunkX, i32 chunkZ, i32 topChunkY) noexcept;
    void clearCoverage() noexcept;
    // True if real chunks hide the whole column.
    [[nodiscard]] bool isCovered(i32 chunkX, i32 chunkZ) const noexcept;
    // The coverage map (R8, one texel per chunk column, wrapping), for the
    // overlay's LOD page.
    [[nodiscard]] rhi::TextureHandle coverageTexture() const noexcept { return m_coverage; }
    // Tile borders (Debug.LOD.TileBorders). Call after the debug draw's beginFrame.
    void drawDebug(DebugDraw& draw) const;
    // Sky light strength (as ChunkRenderer::setDaylight), the darkest
    // brightness (Graphics.Lighting.Brightness) and the face shading
    // (ChunkRenderer::faceShading), so far and near terrain are lit alike.
    void setLight(f32 daylight, f32 brightness, f32 faceShading) noexcept {
        m_daylight = daylight;
        m_brightness = brightness;
        m_faceShading = faceShading;
    }
    void setViewSize(u32 width, u32 height) noexcept;

    void recordUploads(rhi::CommandList& commands);
    void record(rhi::CommandList& commands);
    // Tiles inside the view and indirect draws of the latest recordUploads.
    [[nodiscard]] u32 drawnTiles() const noexcept { return m_drawnTiles; }
    [[nodiscard]] u32 drawCount() const noexcept { return m_drawCount; }
    [[nodiscard]] usize quadCount() const noexcept { return m_quads; }

private:
    // One byte per chunk column: 0 is uncovered, 255 fully covered, anything
    // else a chunk layer plus this offset (lod.frag undoes it).
    static constexpr u8 kCoverageFull = 255;
    static constexpr i32 kCoverageLayerOffset = 65;
    void setCoverage(i32 chunkX, i32 chunkZ, u8 value) noexcept;

    struct Tile {
        OffsetAllocator::Allocation range{};
        u32 slot = 0;
        i64 originX = 0;
        i64 originZ = 0;
        u32 cellSize = 1;
        u32 level = 1;
        i32 baseY = 0;
        i32 minY = 0;
        i32 maxY = 0;
        // Whether every chunk column under the tile is covered (nothing of
        // it would show), as of coverage version `coveredAt`.
        u64 coveredAt = 0;
        bool fullyCovered = false;
    };
    struct PendingCopy {
        usize stagingOffset;
        usize arenaOffset;
        usize bytes;
    };
    struct DeferredFree {
        u64 frame;
        OffsetAllocator::Allocation range;
        u32 slot;
    };

    LodRenderer(const LodRendererDesc& desc);
    void registerStats();
    void release(const Tile& tile);
    void buildDraws();
    [[nodiscard]] bool fullyCovered(Tile& tile) const;

    rhi::Device& m_device;
    stats::StatRegistry* m_stats;
    usize m_arenaBytes;
    u32 m_maxTiles;
    OffsetAllocator m_allocator;
    rhi::BufferHandle m_arena{};
    rhi::BufferHandle m_indices{};
    rhi::BufferHandle m_materials{};
    rhi::TextureHandle m_coverage{};
    rhi::SamplerHandle m_coverageSampler{};
    rhi::PipelineHandle m_pipeline{};
    CpuWriteRing m_staging;
    CpuWriteRing m_tileInfo; // Per frame: each drawn tile's position relative to the camera.
    CpuWriteRing m_draws;    // Per frame: the indirect draws.
    rhi::BufferHandle m_params{};
    Vec4 m_lastParams{-1.0F};

    config::Setting<i32> m_uploadBudgetKb;
    config::Setting<bool> m_coverageDiscard;
    config::Setting<bool> m_colorLevels;
    config::Setting<bool> m_showCoverage;
    config::Setting<bool> m_tileBorders;

    HashMap<u64, Tile> m_tiles;
    std::vector<u32> m_freeSlots;
    std::vector<DeferredFree> m_deferred;
    std::vector<PendingCopy> m_pendingCopies;
    std::span<std::byte> m_stagingMemory;
    usize m_stagingUsed = 0;
    usize m_uploadedBytes = 0;
    u64 m_frame = 0;
    usize m_quads = 0;

    std::vector<u8> m_coverageBytes;
    bool m_coverageDirty = true;
    u64 m_coverageVersion = 1; // Changes with every coverage change; 0 means never checked.
    f32 m_daylight = 1.0F;
    f32 m_brightness = 0.08F;
    f32 m_faceShading = 1.0F;
    Camera m_camera;
    u32 m_viewWidth = 1;
    u32 m_viewHeight = 1;
    u32 m_drawCount = 0;
    u32 m_drawnTiles = 0;
    u32 m_coveredTiles = 0;
    usize m_drawnQuads = 0;
    bool m_drawsBuilt = false;

    struct StatIds {
        stats::StatId tiles;
        stats::StatId quads;
        stats::StatId arenaBytes;
        stats::StatId drawnTiles;
        stats::StatId drawnQuads;
        stats::StatId coveredTiles;
        stats::StatId uploadBytes;
        stats::StatId overBudget;
        stats::StatId outOfMemory;
    } m_statIds;
};

} // namespace vxe::render
