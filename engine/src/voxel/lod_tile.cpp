#include "vxe/voxel/lod_tile.hpp"

#include <vxe/core/assert.hpp>
#include <vxe/io/binary.hpp>
#include <vxe/io/compression.hpp>

#include <algorithm>
#include <format>
#include <limits>

namespace vxe::voxel {
namespace {

constexpr u32 kMagic = 0x544C5856U; // "VXLT" in file byte order.
// The most bytes one span takes in the payload: two 64 bit varints (10 bytes
// each at most) plus 5 fixed bytes.
constexpr usize kMaxSpanBytes = 10 + 10 + 5;
constexpr usize kMaxSpans = usize{kLodCellCount} * kLodMaxSpansPerCell;

// The center of a level's square in tiles of that level: the even tile
// coordinate nearest the camera, so the square moves two tiles at a time.
// (x + T) / 2T rounded down, times two, is "round x / 2T to the nearest
// integer" without floating point.
i32 clipmapCenter(u8 level, i64 block) noexcept {
    const u32 shift = u32{kLodTileCellShift} + level;
    return static_cast<i32>(((block + (i64{1} << shift)) >> (shift + 1)) * 2);
}

} // namespace

bool validLodSpans(std::span<const LodSpan> spans) noexcept {
    if (spans.size() > kLodMaxSpansPerCell) {
        return false;
    }
    for (usize i = 0; i < spans.size(); ++i) {
        if (spans[i].bottomY > spans[i].topY) {
            return false;
        }
        // Strictly below the span above, with at least one block of gap.
        if (i > 0 && i64{spans[i].topY} + 1 >= i64{spans[i - 1].bottomY}) {
            return false;
        }
    }
    return true;
}

LodTile::LodTile()
    : LodTile(LodTileKey{}) {}

LodTile::LodTile(LodTileKey key)
    : m_key(key)
    , m_first(kLodCellCount + 1, 0) {}

usize LodTile::memoryBytes() const noexcept {
    return (m_first.capacity() * sizeof(u16)) + (m_spans.capacity() * sizeof(LodSpan));
}

void LodTile::setCell(u32 index, std::span<const LodSpan> spans) {
    VXE_ASSERT(index < kLodCellCount, "LOD cell index out of range");
    VXE_ASSERT(validLodSpans(spans), "Invalid LOD spans");
    const u16 begin = m_first[index];
    const u16 oldCount = static_cast<u16>(m_first[index + 1] - begin);
    const auto at = m_spans.begin() + begin;
    // Overwrite what fits, then insert or erase the difference.
    const usize common = std::min<usize>(oldCount, spans.size());
    std::copy_n(spans.begin(), common, at);
    if (spans.size() > oldCount) {
        m_spans.insert(at + oldCount, spans.begin() + static_cast<std::ptrdiff_t>(oldCount), spans.end());
    } else {
        m_spans.erase(at + static_cast<std::ptrdiff_t>(spans.size()), at + oldCount);
    }
    const i32 change = static_cast<i32>(spans.size()) - oldCount;
    for (u32 i = index + 1; i <= kLodCellCount; ++i) {
        m_first[i] = static_cast<u16>(m_first[i] + change);
    }
}

LodTileBuilder::LodTileBuilder(LodTileKey key)
    : m_tile(key) {}

void LodTileBuilder::addCell(std::span<const LodSpan> spans) {
    VXE_ASSERT(m_next < kLodCellCount, "Too many LOD cells");
    VXE_ASSERT(validLodSpans(spans), "Invalid LOD spans");
    m_tile.m_spans.insert(m_tile.m_spans.end(), spans.begin(), spans.end());
    ++m_next;
    m_tile.m_first[m_next] = static_cast<u16>(m_tile.m_spans.size());
}

LodTile LodTileBuilder::build() && {
    VXE_ASSERT(m_next == kLodCellCount, "A LOD tile needs all of its cells");
    // Tiles live long (hundreds stay resident), so drop the growth slack:
    // one copy now instead of up to half the span memory unused.
    m_tile.m_spans.shrink_to_fit();
    return std::move(m_tile);
}

void encodeLodTile(const LodTile& tile, u64 worldHash, std::vector<std::byte>& out) {
    io::BinaryWriter payload;
    for (u32 i = 0; i < kLodCellCount; ++i) {
        payload.writeU8(static_cast<u8>(tile.cellAt(i).size()));
    }
    i64 previousTop = 0;
    for (const LodSpan& span : tile.allSpans()) {
        payload.writeVarI64(i64{span.topY} - previousTop);
        payload.writeVarU64(static_cast<u64>(i64{span.topY} - span.bottomY));
        payload.writeU16(span.topMaterial);
        payload.writeU16(span.sideMaterial);
        payload.writeU8(span.skyLight);
        previousTop = span.topY;
    }
    auto compressed = io::compress(payload.bytes(), io::Codec::Lz4);
    VXE_VERIFY(compressed.hasValue(), "LZ4 compression of a LOD tile failed");

    io::BinaryWriter header;
    header.writeU32(kMagic);
    header.writeU16(kLodTileFormat);
    header.writeU16(0);
    header.writeU64(worldHash);
    header.writeU8(tile.key().level);
    header.writeI32(tile.key().x);
    header.writeI32(tile.key().z);
    header.writeU32(static_cast<u32>(tile.spanCount()));
    header.writeU32(static_cast<u32>(payload.size()));
    header.writeU32(static_cast<u32>(compressed.value().size()));
    header.writeU64(hashBytes(compressed.value()));
    out.insert(out.end(), header.bytes().begin(), header.bytes().end());
    out.insert(out.end(), compressed.value().begin(), compressed.value().end());
}

namespace {

// Reads the spans of the payload into a tile; an Error on anything that does
// not add up.
Result<LodTile> readPayload(std::span<const std::byte> raw, const LodTileKey& key, u32 spanCount) {
    io::BinaryReader reader(raw);
    const std::span<const std::byte> counts = reader.readBytes(kLodCellCount);
    if (!reader.ok()) {
        return Error(ErrorCode::ParseError, "LOD tile payload is truncated");
    }
    usize total = 0;
    for (const std::byte count : counts) {
        if (static_cast<usize>(count) > kLodMaxSpansPerCell) {
            return Error(ErrorCode::ParseError, "LOD tile has a cell with too many spans");
        }
        total += static_cast<usize>(count);
    }
    if (total != spanCount) {
        return Error(ErrorCode::ParseError, "LOD tile span counts do not add up");
    }
    LodTileBuilder builder(key);
    std::array<LodSpan, kLodMaxSpansPerCell> cell{};
    i64 previousTop = 0;
    for (const std::byte count : counts) {
        const auto spans = std::span(cell).first(static_cast<usize>(count));
        for (LodSpan& span : spans) {
            const i64 top = previousTop + reader.readVarI64();
            const u64 height = reader.readVarU64();
            const i64 bottom = top - static_cast<i64>(std::min<u64>(height, u64{1} << 40U));
            if (top > std::numeric_limits<i32>::max() || bottom < std::numeric_limits<i32>::min()) {
                return Error(ErrorCode::ParseError, "LOD tile span is out of range");
            }
            span.topY = static_cast<i32>(top);
            span.bottomY = static_cast<i32>(bottom);
            span.topMaterial = reader.readU16();
            span.sideMaterial = reader.readU16();
            span.skyLight = reader.readU8();
            previousTop = top;
        }
        if (!reader.ok() || !validLodSpans(spans) ||
            std::ranges::any_of(spans, [](const LodSpan& span) { return span.skyLight > 15; })) {
            return Error(ErrorCode::ParseError, "LOD tile has damaged spans");
        }
        builder.addCell(spans);
    }
    if (!reader.atEnd()) {
        return Error(ErrorCode::ParseError, "LOD tile payload has trailing bytes");
    }
    return std::move(builder).build();
}

} // namespace

Result<LodTile> decodeLodTile(std::span<const std::byte> bytes, u64 worldHash, const LodTileKey& expected) {
    io::BinaryReader reader(bytes);
    const u32 magic = reader.readU32();
    const u16 format = reader.readU16();
    const u16 flags = reader.readU16();
    const u64 storedWorld = reader.readU64();
    const LodTileKey key{.level = reader.readU8(), .x = reader.readI32(), .z = reader.readI32()};
    const u32 spanCount = reader.readU32();
    const u32 rawSize = reader.readU32();
    const u32 compressedSize = reader.readU32();
    const u64 checksum = reader.readU64();
    if (!reader.ok() || magic != kMagic) {
        return Error(ErrorCode::ParseError, "Not a LOD tile file");
    }
    if (format != kLodTileFormat || flags != 0) {
        return Error(ErrorCode::VersionMismatch,
                     std::format("LOD tile format {} is not {}", format, kLodTileFormat));
    }
    if (storedWorld != worldHash) {
        return Error(ErrorCode::VersionMismatch, "LOD tile was made for another world or generator");
    }
    if (key != expected) {
        return Error(ErrorCode::InvalidArgument,
                     std::format("LOD tile file holds tile {} {} {}, not {} {} {}", key.level, key.x, key.z,
                                 expected.level, expected.x, expected.z));
    }
    if (spanCount > kMaxSpans || rawSize < kLodCellCount ||
        rawSize > kLodCellCount + (usize{spanCount} * kMaxSpanBytes) ||
        compressedSize != reader.remaining()) {
        return Error(ErrorCode::ParseError, "LOD tile header sizes are damaged");
    }
    const std::span<const std::byte> compressed = reader.readBytes(compressedSize);
    if (hashBytes(compressed) != checksum) {
        return Error(ErrorCode::ParseError, "LOD tile checksum does not match");
    }
    auto raw = io::decompress(compressed, io::Codec::Lz4, rawSize);
    if (!raw) {
        return Error(ErrorCode::ParseError, std::format("LOD tile payload: {}", raw.error().message()));
    }
    return readPayload(raw.value(), key, spanCount);
}

LodClipmap::LodClipmap(const LodClipmapDesc& desc) {
    setDesc(desc);
}

void LodClipmap::setDesc(const LodClipmapDesc& desc) {
    VXE_ASSERT(desc.halfExtentTiles >= 2 && desc.halfExtentTiles % 2 == 0,
               "The clipmap half extent must be even and at least 2");
    VXE_ASSERT(desc.firstLevel >= kLodMinLevel && desc.levels >= 1 &&
                   desc.firstLevel + desc.levels - 1 <= kLodMaxLevel,
               "Clipmap levels out of range");
    m_desc = desc;
    m_first = true;
}

void LodClipmap::tilesAround(const LodClipmapDesc& desc, i64 blockX, i64 blockZ,
                             std::vector<LodTileKey>& out) {
    const i32 half = desc.halfExtentTiles;
    for (u8 i = 0; i < desc.levels; ++i) {
        const auto level = static_cast<u8>(desc.firstLevel + i);
        const i32 centerX = clipmapCenter(level, blockX);
        const i32 centerZ = clipmapCenter(level, blockZ);
        // The square of the finer level, in tiles of this level. Its center is
        // even in its own tiles, so halving it gives whole tiles here.
        i32 holeX0 = 0;
        i32 holeX1 = 0;
        i32 holeZ0 = 0;
        i32 holeZ1 = 0;
        if (i > 0) {
            const auto finer = static_cast<u8>(level - 1);
            holeX0 = (clipmapCenter(finer, blockX) / 2) - (half / 2);
            holeZ0 = (clipmapCenter(finer, blockZ) / 2) - (half / 2);
            holeX1 = holeX0 + half;
            holeZ1 = holeZ0 + half;
        }
        for (i32 z = centerZ - half; z < centerZ + half; ++z) {
            for (i32 x = centerX - half; x < centerX + half; ++x) {
                if (x >= holeX0 && x < holeX1 && z >= holeZ0 && z < holeZ1) {
                    continue;
                }
                out.push_back({.level = level, .x = x, .z = z});
            }
        }
    }
}

bool LodClipmap::update(i64 blockX, i64 blockZ) {
    // Fast path: the tiles only change when some level's center moves.
    bool moved = m_first;
    for (u8 i = 0; i < m_desc.levels; ++i) {
        const auto level = static_cast<u8>(m_desc.firstLevel + i);
        const i32 centerX = clipmapCenter(level, blockX);
        const i32 centerZ = clipmapCenter(level, blockZ);
        const usize slot = usize{2} * level;
        if (m_centers.at(slot) != centerX || m_centers.at(slot + 1) != centerZ) {
            m_centers.at(slot) = centerX;
            m_centers.at(slot + 1) = centerZ;
            moved = true;
        }
    }
    m_added.clear();
    m_removed.clear();
    if (!moved) {
        return false;
    }
    m_first = false;
    m_scratch.clear();
    tilesAround(m_desc, blockX, blockZ, m_scratch);
    for (const LodTileKey& key : m_scratch) {
        if (!m_set.contains(key)) {
            m_added.push_back(key);
        }
    }
    HashSet<LodTileKey, KeyHash> next(m_scratch.begin(), m_scratch.end());
    for (const LodTileKey& key : m_tiles) {
        if (!next.contains(key)) {
            m_removed.push_back(key);
        }
    }
    m_set = std::move(next);
    std::swap(m_tiles, m_scratch);
    return !m_added.empty() || !m_removed.empty();
}

bool LodClipmap::contains(const LodTileKey& key) const noexcept {
    return m_set.contains(key);
}

} // namespace vxe::voxel
