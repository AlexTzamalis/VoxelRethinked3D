#pragma once

#include <vxe/core/result.hpp>
#include <vxe/core/stats.hpp>
#include <vxe/core/types.hpp>

#include <array>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace vxe::voxel {

// The runtime id of one block state: a block plus its properties, for
// example a log with a given axis (Section 9.3). Saves never store these;
// they store names, so ids may change between versions.
using BlockStateId = u16;

// State 0 is always air: invisible, lets all light through. Every registry
// has it, so freshly created chunk storage (all zeros) is empty space.
inline constexpr BlockStateId kAirState = 0;
inline constexpr usize kMaxBlockStates = usize{std::numeric_limits<BlockStateId>::max()} + 1;

// Which render pass draws a block and how it hides its neighbors' faces.
enum class RenderLayer : u8 {
    Invisible,   // Air and similar. Never meshed.
    Opaque,      // Fully solid, hides every neighbor face behind it.
    Cutout,      // Alpha tested (leaves, plants). Drawn in the cutout pass.
    Translucent, // Alpha blended (water, glass). Drawn last, sorted.
    Plant,       // Two crossed planes (grass, flowers). Hides nothing.
};

// Face order everywhere in the engine: +X, -X, +Y, -Y, +Z, -Z.
enum class BlockFace : u8 {
    PosX,
    NegX,
    PosY,
    NegY,
    PosZ,
    NegZ
};
inline constexpr usize kBlockFaceCount = 6;

inline constexpr u8 kMaxLightLevel = 15;

// Tint kinds: 0 none, then what the game means by them (grass, foliage, water, ...).
inline constexpr usize kTintCount = 16;

// How one texture layer is colored, one u32 per layer in the renderer's table
// (ChunkRenderer::setTextureLooks). A tint belongs to a texture and not to a
// quad, so a block can tint its top but not its underside, faces merge as
// before and quads stay 8 bytes. `overlayLayer` is the layer drawn over this
// one on the same face, plus 1 (0 for none); with one, usually only the
// overlay is tinted, so a grey fringe colors a plain dirt side.
//   bits 0..15 overlayLayer, 16..19 tint, 20..23 overlayTint
[[nodiscard]] constexpr u32 textureLook(u32 overlayLayer, u32 tint, u32 overlayTint) noexcept {
    return overlayLayer | (tint << 16U) | (overlayTint << 20U);
}

// Everything the engine needs to know about a block state to mesh and light
// it (Section 6.3). The game fills these in from its data files; the engine
// never needs the block's name.
struct BlockRenderInfo {
    RenderLayer layer = RenderLayer::Invisible;
    u8 emittedLight = 0;                                 // 0 to 15, for the block light flood fill.
    u8 lightOpacity = kMaxLightLevel;                    // Light lost passing through, 15 blocks it fully.
    bool cullsSameType = true;                           // Glass next to glass hides the shared face.
    std::array<u16, kBlockFaceCount> faceTextureLayer{}; // Texture array layer per face (BlockFace order).
    u8 tintIndex = 0; // 0 is no tint, otherwise the block's tint kind (textureLook).
    // Plants: textures in a row from the face's layer. The block's position
    // picks one, so each spot always looks the same and nothing is stored.
    u8 variationCount = 1;
    // Leaves: the layer plus 1 of bushy planes for MeshOptions::advancedLeaves.
    u16 bushyLayer = 0;

    [[nodiscard]] constexpr bool isOpaque() const noexcept { return layer == RenderLayer::Opaque; }
    [[nodiscard]] constexpr bool isVisible() const noexcept { return layer != RenderLayer::Invisible; }
    [[nodiscard]] constexpr u16 textureLayer(BlockFace face) const noexcept {
        return faceTextureLayer[static_cast<usize>(face)];
    }
    friend constexpr bool operator==(const BlockRenderInfo&, const BlockRenderInfo&) = default;
};

// The render info of air (state 0).
[[nodiscard]] constexpr BlockRenderInfo airRenderInfo() noexcept {
    return BlockRenderInfo{.layer = RenderLayer::Invisible, .lightOpacity = 0, .cullsSameType = false};
}

// A compact, read only table of every block state's render info. It never
// changes after it is built, so meshing and lighting jobs on any thread read
// it without locks. When block data or texture packs reload, a new view is
// built and published (BlockRegistry); jobs that still hold the old one keep
// using it safely until they finish.
class BlockRegistryView {
public:
    [[nodiscard]] const BlockRenderInfo& info(BlockStateId state) const noexcept {
        // Unknown states (for example from a newer save) read as air instead
        // of out of bounds memory. One compare, well predicted.
        return state < m_infos.size() ? m_infos[state] : m_infos[kAirState];
    }
    [[nodiscard]] usize size() const noexcept { return m_infos.size(); }
    // Counts publications: a mesher can compare it to know its cached
    // results used older block data.
    [[nodiscard]] u64 generation() const noexcept { return m_generation; }

private:
    friend class BlockRegistryBuilder;
    friend class BlockRegistry;
    std::vector<BlockRenderInfo> m_infos;
    u64 m_generation = 0;
};

// Collects block states and builds a BlockRegistryView. State 0 is air from
// the start; add() hands out the following ids in order.
//
// Thread safety: one thread at a time (usually the loader).
class BlockRegistryBuilder {
public:
    BlockRegistryBuilder();

    // Adds a state and returns its id. Fails for light values above 15 and
    // when all 65,536 ids are used.
    [[nodiscard]] Result<BlockStateId> add(const BlockRenderInfo& info);
    [[nodiscard]] usize size() const noexcept { return m_infos.size(); }

    // The finished table, ready for BlockRegistry::publish. A view has no
    // public way to change it, so handing out a non const pointer only lets
    // publish() stamp its generation. The builder can be reused afterwards.
    [[nodiscard]] std::shared_ptr<BlockRegistryView> build() const;

private:
    std::vector<BlockRenderInfo> m_infos;
};

// Holds the current BlockRegistryView and swaps it atomically.
//
// Readers call current() once per job (not per block) and keep the returned
// pointer for the job's lifetime, so a publish never pulls data out from
// under a running job. The swap is guarded by a mutex held only for a
// pointer copy: std::atomic<std::shared_ptr> is not available in every
// standard library the engine builds with (libc++), and the older atomic
// free functions are deprecated in C++20.
//
// Starts with a view that only knows air. Stat (optional):
// Voxel.BlockRegistry.States.
//
// Thread safety: current() from any thread; publish() from any thread.
class BlockRegistry {
public:
    explicit BlockRegistry(stats::StatRegistry* stats = nullptr);

    [[nodiscard]] std::shared_ptr<const BlockRegistryView> current() const;
    // Gives `view` the next generation number and makes it the current one.
    // The view must not be published twice.
    void publish(std::shared_ptr<BlockRegistryView> view);
    [[nodiscard]] u64 generation() const;

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<const BlockRegistryView> m_current;
    u64 m_generation = 0;
    stats::StatRegistry* m_stats;
    stats::StatId m_statesStat;
};

} // namespace vxe::voxel
