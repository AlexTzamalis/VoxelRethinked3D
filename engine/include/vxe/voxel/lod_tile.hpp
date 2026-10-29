#pragma once

#include <vxe/core/containers.hpp>
#include <vxe/core/hash.hpp>
#include <vxe/core/result.hpp>
#include <vxe/core/types.hpp>

#include <array>
#include <compare>
#include <span>
#include <vector>

// Far LOD data (Section 13.2): tiles of 64 x 64 cells, each cell a short list
// of vertical spans of terrain, at levels where one cell is 2^level blocks
// wide. The world generator's column sampler fills them (E11.2); the LOD
// mesher turns them into far terrain (E11.3).
namespace vxe::voxel {

inline constexpr i32 kLodTileCells = 64; // Cells along each side of a tile.
inline constexpr i32 kLodTileCellShift = 6;
inline constexpr u32 kLodCellCount = u32{kLodTileCells} * kLodTileCells;
// Level 1 has cells of 2 blocks; level 0 would be real chunks. The top level
// (cells of 65,536 blocks, tiles of over 4 million) is far beyond any view.
inline constexpr u8 kLodMinLevel = 1;
inline constexpr u8 kLodMaxLevel = 16;
// The most spans a cell can hold. Graphics.LOD.MaxSpansPerCell (4 by
// default) picks how many the sampler keeps; this is what storage allows. It
// keeps a tile's span offsets within 16 bits (4096 cells x 8 = 32,768).
inline constexpr usize kLodMaxSpansPerCell = 8;

// Which tile: its level and its position in the grid of that level's tiles.
// Tile (x, z) of level L covers blocks x * T to x * T + T - 1 (and the same
// along Z), where T = 64 * 2^L is the tile size in blocks.
//
// The tiles form a quadtree: a tile of level L + 1 covers exactly the 2 x 2
// tiles of level L below it (its children). Edits flow up this tree (E11.5).
struct LodTileKey {
    u8 level = kLodMinLevel;
    i32 x = 0;
    i32 z = 0;

    [[nodiscard]] constexpr i64 cellSize() const noexcept { return i64{1} << level; }
    [[nodiscard]] constexpr i64 tileSize() const noexcept { return i64{kLodTileCells} << level; }
    // The block coordinates of the tile's -X -Z corner.
    [[nodiscard]] constexpr i64 originX() const noexcept { return i64{x} * tileSize(); }
    [[nodiscard]] constexpr i64 originZ() const noexcept { return i64{z} * tileSize(); }
    // The tile one level up that contains this one. The arithmetic shift
    // rounds towards negative infinity, so tile -1 has parent -1.
    [[nodiscard]] constexpr LodTileKey parent() const noexcept {
        return {.level = static_cast<u8>(level + 1), .x = x >> 1, .z = z >> 1};
    }
    // One of the 4 tiles one level down, `index` 0 to 3: bit 0 picks +X,
    // bit 1 picks +Z.
    [[nodiscard]] constexpr LodTileKey child(u32 index) const noexcept {
        return {.level = static_cast<u8>(level - 1),
                .x = (x * 2) + static_cast<i32>(index & 1U),
                .z = (z * 2) + static_cast<i32>((index >> 1U) & 1U)};
    }

    friend constexpr auto operator<=>(const LodTileKey&, const LodTileKey&) = default;
};

// The tile of `level` holding block column (x, z). Blocks far beyond the
// int32 tile grid of level 1 (over 2.7 * 10^11 blocks) are clamped; no world
// gets there (Section 9.1).
[[nodiscard]] constexpr LodTileKey lodTileAt(u8 level, i64 blockX, i64 blockZ) noexcept {
    const u32 shift = u32{kLodTileCellShift} + level;
    return {.level = level, .x = static_cast<i32>(blockX >> shift), .z = static_cast<i32>(blockZ >> shift)};
}

// A vertical run of terrain in one cell, from the block at bottomY to the
// block at topY (both included). Materials are indices into the pack derived
// color table (Section 13.2); what an index means is up to the game.
struct LodSpan {
    i32 bottomY = 0;
    i32 topY = 0;
    u16 topMaterial = 0;          // Seen from above.
    u16 sideMaterial = 0;         // Seen from the side, where a neighbor cell is lower.
    u8 skyLight = 15;             // Sky light just above the top (0 to 15).
    std::array<u8, 3> reserved{}; // Explicit, so a span has no padding.

    friend constexpr bool operator==(const LodSpan&, const LodSpan&) = default;
};
static_assert(sizeof(LodSpan) == 16);

// True if spans of one cell are valid: at most kLodMaxSpansPerCell, each with
// bottomY <= topY, ordered from the highest down, and not touching (a gap of
// at least one block between them; touching spans are one span).
[[nodiscard]] bool validLodSpans(std::span<const LodSpan> spans) noexcept;

// The terrain of one tile: the spans of its 4096 cells, cell (cx, cz) at
// index cz * 64 + cx. A cell without spans is empty (air all the way, or
// void). The highest span comes first, because that is the one seen.
//
// Storage is flat: all spans in one array plus the offset of each cell's
// first span (4097 offsets of 16 bits), so a typical tile is one allocation
// of a few tens of kilobytes with no per cell overhead.
//
// Thread safety: a plain value. Build it on one thread, then share it
// read only or move it.
class LodTile {
public:
    LodTile();
    explicit LodTile(LodTileKey key);

    [[nodiscard]] const LodTileKey& key() const noexcept { return m_key; }
    [[nodiscard]] std::span<const LodSpan> cell(u32 cellX, u32 cellZ) const noexcept {
        return cellAt((cellZ * u32{kLodTileCells}) + cellX);
    }
    [[nodiscard]] std::span<const LodSpan> cellAt(u32 index) const noexcept {
        return std::span(m_spans).subspan(m_first[index], m_first[index + 1] - m_first[index]);
    }
    [[nodiscard]] usize spanCount() const noexcept { return m_spans.size(); }
    [[nodiscard]] std::span<const LodSpan> allSpans() const noexcept { return m_spans; }
    // Heap bytes held (spans and offsets), for the memory stats.
    [[nodiscard]] usize memoryBytes() const noexcept;

    // Replaces the spans of one cell; they must pass validLodSpans. Moves the
    // spans of every later cell, so filling a whole tile this way is slow; the
    // builder below is the fast way. Meant for edits (E11.5).
    void setCell(u32 index, std::span<const LodSpan> spans);

    friend bool operator==(const LodTile&, const LodTile&) = default;

private:
    friend class LodTileBuilder;

    LodTileKey m_key{};
    std::vector<u16> m_first; // kLodCellCount + 1 offsets into m_spans.
    std::vector<LodSpan> m_spans;
};

// Fills a tile cell after cell, in index order (cz * 64 + cx), without
// moving anything: what the column sampler does.
class LodTileBuilder {
public:
    explicit LodTileBuilder(LodTileKey key);

    // Adds the next cell. The spans must pass validLodSpans.
    void addCell(std::span<const LodSpan> spans);
    [[nodiscard]] u32 cellsAdded() const noexcept { return m_next; }
    // The tile; every cell must have been added.
    [[nodiscard]] LodTile build() &&;

private:
    LodTile m_tile;
    u32 m_next = 0;
};

// Identifies the world a cached tile was made for (Section 13.4 cache key):
// the seed, the generator's version and the hash of its preset. A tile
// cached for other values is stale and generated again.
[[nodiscard]] constexpr u64 lodWorldHash(u64 seed, u32 generatorVersion, u64 presetHash) noexcept {
    return hashCombine(hashCombine(hashCombine(0x4C4F4421ULL, seed), generatorVersion), presetHash);
}

// The cache file format (version 1), little endian:
//   u32 magic 'VXLT', u16 format, u16 flags (0)
//   u64 world hash, u8 level, i32 tile x, i32 tile z
//   u32 span count, u32 payload size before compression, u32 compressed size
//   u64 XXH3 of the compressed payload
//   LZ4 payload: 4096 span counts (u8), then per span: varint top delta from
//   the previous span's top, varint height, u16 top and side material, u8
//   sky light. Neighboring cells have similar heights, so the deltas are
//   small and the payload compresses well.
inline constexpr u16 kLodTileFormat = 1;

// Appends the cache file bytes of `tile` to `out`.
void encodeLodTile(const LodTile& tile, u64 worldHash, std::vector<std::byte>& out);

// Reads a tile back. Fails with VersionMismatch if the file is from another
// format, world or generator (stale, generate it again), with ParseError if it
// is damaged, and with InvalidArgument if it holds a different tile than
// `expected`. Never reads out of bounds, whatever the bytes are.
[[nodiscard]] Result<LodTile> decodeLodTile(std::span<const std::byte> bytes, u64 worldHash,
                                            const LodTileKey& expected);

struct LodClipmapDesc {
    u8 firstLevel = kLodMinLevel; // The finest level, nearest the camera.
    u8 levels = 6;                // How many levels (Section 13.3 "High" uses 6).
    // Each level covers 2 x halfExtent tiles along each side, around the
    // camera. Even, at least 2; 4 or more keeps at least one tile of every
    // level between its finer and coarser neighbors on every side.
    i32 halfExtentTiles = 4;
};

// The quadtree clipmap of LOD tiles around the camera (Section 13.1): every
// level covers a square of tiles around the camera, with a hole where the
// finer level below covers the ground, so each block column is drawn by
// exactly one tile. The finest level has no hole (real chunks are drawn over
// it, and it shows where they are not ready yet).
//
// The squares move in steps of two tiles of their level, so the hole of a
// level is always made of whole tiles of the level around it, and a level
// only changes when the camera has moved a full tile of that level: far
// levels change rarely.
//
// Reach: the coarsest level ends between (halfExtent - 1) and halfExtent of
// its tiles from the camera. With the defaults (levels 1 to 6, half extent 4)
// that is 12,288 to 16,384 blocks, with 304 tiles.
//
// Thread safety: a plain value, owned by one thread.
class LodClipmap {
public:
    explicit LodClipmap(const LodClipmapDesc& desc);

    // Moves the clipmap to the camera's block column. Returns true if the
    // tile set changed; added() and removed() then hold the difference to the
    // previous call (the first call adds everything).
    bool update(i64 blockX, i64 blockZ);
    // Changes the shape; the next update() reports the whole difference.
    void setDesc(const LodClipmapDesc& desc);

    [[nodiscard]] const LodClipmapDesc& desc() const noexcept { return m_desc; }
    [[nodiscard]] std::span<const LodTileKey> tiles() const noexcept { return m_tiles; }
    [[nodiscard]] std::span<const LodTileKey> added() const noexcept { return m_added; }
    [[nodiscard]] std::span<const LodTileKey> removed() const noexcept { return m_removed; }
    [[nodiscard]] bool contains(const LodTileKey& key) const noexcept;

    // The tiles of a clipmap around a block column, appended to `out` from
    // the finest level up. The same set update() computes.
    static void tilesAround(const LodClipmapDesc& desc, i64 blockX, i64 blockZ, std::vector<LodTileKey>& out);

private:
    struct KeyHash {
        [[nodiscard]] u64 operator()(const LodTileKey& key) const noexcept {
            return hashCombine(hashCombine(key.level, static_cast<u32>(key.x)), static_cast<u32>(key.z));
        }
    };

    LodClipmapDesc m_desc;
    bool m_first = true;
    std::array<i32, 2 * (kLodMaxLevel + 1)> m_centers{}; // Per level: center tile x and z.
    std::vector<LodTileKey> m_tiles;
    std::vector<LodTileKey> m_added;
    std::vector<LodTileKey> m_removed;
    std::vector<LodTileKey> m_scratch;
    HashSet<LodTileKey, KeyHash> m_set;
};

} // namespace vxe::voxel
