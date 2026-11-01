#include "rivers.hpp"

#include <vxe/math/noise.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <future>
#include <queue>
#include <utility>

namespace vr {

using vxe::f32;
using vxe::f64;
using vxe::i32;
using vxe::i64;
using vxe::u32;
using vxe::u64;
using vxe::usize;

namespace {

constexpr i64 kCell = 48;       // Blocks per side of a drainage cell.
constexpr i64 kTileCells = 170; // Cells per side of a tile.
constexpr i64 kTileSize = kCell * kTileCells;
constexpr i64 kWindowCells = kTileCells * 3; // A tile sees itself and its 8 neighbors.
constexpr u32 kNoNext = 0xFFFFFFFFU;
constexpr f32 kFlood = 0.01F;    // Rise per cell in filled hollows, so water still has a way.
constexpr u32 kFirstStream = 14; // Cells of rain that make a stream.
constexpr f64 kWidest = 22.0;    // Blocks across the largest river.
constexpr f32 kLakeDepth = 6.0F; // A hollow this deep or more holds a lake.
constexpr f64 kBelowPlan = 6.0;  // Blocks a river runs below the planned ground.
constexpr i64 kBucket = 240;     // Blocks per side of a bucket of the stretch index.
constexpr i64 kBuckets = kTileSize / kBucket;
constexpr usize kCachedHeights = 64;
constexpr usize kCachedTiles = 32;
constexpr f32 kNoLake = -1.0e6F;
constexpr u64 kSpotSalt = 0x51ED270B7A1CE5ULL;

i64 floorDiv(i64 value, i64 divisor) noexcept {
    return value >= 0 ? value / divisor : ((value + 1) / divisor) - 1;
}

// Where a cell's piece of river passes: off the middle of the cell by a hash of
// the cell alone, so rivers wander instead of following the grid, and every
// tile agrees.
struct Spot {
    f64 x = 0.0;
    f64 z = 0.0;
};
Spot spotOf(i64 cellX, i64 cellZ) noexcept {
    const u64 h = vxe::noise::hash2(kSpotSalt, cellX, cellZ);
    const f64 half = static_cast<f64>(kCell) * 0.5;
    return {.x = (static_cast<f64>(cellX) + 0.5) * static_cast<f64>(kCell) +
                 ((vxe::noise::unit(h) - 0.5) * half * 1.3),
            .z = (static_cast<f64>(cellZ) + 0.5) * static_cast<f64>(kCell) +
                 ((vxe::noise::unit(h * 0x9E3779B97F4A7C15ULL) - 0.5) * half * 1.3)};
}

// How wide a river is that carries the rain of this many cells. The square
// root is how real rivers grow: ten times the land, about three times the width.
f64 widthOf(u32 cells) noexcept {
    const f64 extra = static_cast<f64>(cells - std::min(cells, kFirstStream));
    return std::min(1.2 + (std::sqrt(extra) * 0.55), kWidest);
}

// The water surface of a river running through ground filled to this height.
// Near the sea it meets the sea.
f64 riverLevel(f32 filled) noexcept {
    return std::max(static_cast<f64>(std::floor(filled)) - kBelowPlan, 0.0);
}

// Every network gets its own number: the next world's network can land at the
// same address as the last one's, and must not see its tiles.
u64 nextNetworkId() noexcept {
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

struct Open {
    f32 height = 0.0F;
    u32 cell = 0;
    u32 order = 0; // Ties go first in, first out, so the result is exact.
    bool operator<(const Open& other) const noexcept {
        return height != other.height ? height > other.height : order > other.order;
    }
};

} // namespace

struct RiverNetwork::Heights {
    std::vector<f32> cells; // kTileCells squared, row by row.
};

struct RiverNetwork::Tile {
    std::vector<Stretch> stretches;
    std::vector<std::vector<u32>> buckets; // kBuckets squared.
    std::vector<f32> lakes;                // kTileCells squared; kNoLake where there is none.
};

std::size_t RiverNetwork::TileKeyHash::operator()(const TileKey& key) const noexcept {
    return static_cast<std::size_t>(vxe::noise::hash2(0, key.x, key.z));
}

RiverNetwork::RiverNetwork(const Planet& planet)
    : m_planet(&planet)
    , m_id(nextNetworkId()) {}

RiverNetwork::~RiverNetwork() = default;

std::shared_ptr<const RiverNetwork::Heights> RiverNetwork::buildHeights(TileKey key) const {
    auto out = std::make_shared<Heights>();
    out->cells.resize(static_cast<usize>(kTileCells * kTileCells));
    for (i64 z = 0; z < kTileCells; ++z) {
        for (i64 x = 0; x < kTileCells; ++x) {
            const f64 wx = (static_cast<f64>((key.x * kTileCells) + x) + 0.5) * static_cast<f64>(kCell);
            const f64 wz = (static_cast<f64>((key.z * kTileCells) + z) + 0.5) * static_cast<f64>(kCell);
            out->cells[static_cast<usize>((z * kTileCells) + x)] =
                static_cast<f32>(m_planet->drainageHeight(wx, wz));
        }
    }
    return out;
}

template <typename Value, typename Build>
std::shared_ptr<const Value> fetch(auto& cache, auto key, usize capacity, Build&& build) {
    using SlotPtr = std::decay_t<decltype(cache.slots.begin()->second)>;
    SlotPtr slot;
    {
        const std::scoped_lock lock(cache.mutex);
        auto& entry = cache.slots[key];
        if (!entry) {
            entry = std::make_shared<std::decay_t<decltype(*entry)>>();
            cache.order.push_back(key);
            if (cache.order.size() > capacity) {
                cache.slots.erase(cache.order.front());
                cache.order.pop_front();
            }
        }
        slot = entry;
    }
    // Outside the lock: building takes a while, and others need other tiles.
    std::call_once(slot->once, [&] { slot->value = build(); });
    return slot->value;
}

std::shared_ptr<const RiverNetwork::Heights> RiverNetwork::heights(TileKey key) const {
    return fetch<Heights>(m_heights, key, kCachedHeights, [&] { return buildHeights(key); });
}

std::shared_ptr<const RiverNetwork::Tile> RiverNetwork::tile(TileKey key) const {
    // Nearly every question asks about the tile before, and terrain jobs ask
    // thousands of times per chunk: the last answer per thread saves them all
    // from queueing at the lock.
    struct Last {
        u64 owner = 0;
        TileKey key;
        std::shared_ptr<const Tile> tile;
    };
    thread_local Last last;
    if (last.owner != m_id || last.key != key || !last.tile) {
        last = {.owner = m_id, .key = key, .tile = fetch<Tile>(m_tiles, key, kCachedTiles, [&] {
                                               return buildTile(key);
                                           })};
    }
    return last.tile;
}

std::shared_ptr<const RiverNetwork::Tile> RiverNetwork::buildTile(TileKey key) const {
    const auto count = static_cast<usize>(kWindowCells * kWindowCells);
    std::vector<f32> height(count);
    // The 9 height tiles are most of the work, and each stands alone, so they
    // run side by side: every chunk job in the area waits for this tile.
    std::array<std::future<std::shared_ptr<const Heights>>, 9> parts;
    for (i64 dz = -1; dz <= 1; ++dz) {
        for (i64 dx = -1; dx <= 1; ++dx) {
            parts.at(static_cast<usize>(((dz + 1) * 3) + dx + 1)) =
                std::async(std::launch::async,
                           [this, key, dx, dz] { return heights({.x = key.x + dx, .z = key.z + dz}); });
        }
    }
    for (i64 dz = -1; dz <= 1; ++dz) {
        for (i64 dx = -1; dx <= 1; ++dx) {
            const auto part = parts.at(static_cast<usize>(((dz + 1) * 3) + dx + 1)).get();
            for (i64 z = 0; z < kTileCells; ++z) {
                const auto row = static_cast<usize>((((dz + 1) * kTileCells) + z) * kWindowCells +
                                                    ((dx + 1) * kTileCells));
                std::copy_n(part->cells.begin() + (z * kTileCells), kTileCells,
                            height.begin() + static_cast<std::ptrdiff_t>(row));
            }
        }
    }
    // Priority flood: start from the sea and the edge of the window, always
    // grow from the lowest open cell. A cell reached that way drains into the
    // cell it was reached from, and its surface can never lie below that one:
    // a hollow fills up to where it spills, which is what water does.
    std::vector<f32> filled(height);
    std::vector<u32> next(count, kNoNext);
    std::vector<bool> seen(count, false);
    std::vector<u32> visits;
    visits.reserve(count);
    std::priority_queue<Open> open;
    u32 order = 0;
    for (i64 z = 0; z < kWindowCells; ++z) {
        for (i64 x = 0; x < kWindowCells; ++x) {
            const auto cell = static_cast<usize>((z * kWindowCells) + x);
            const bool edge = x == 0 || z == 0 || x == kWindowCells - 1 || z == kWindowCells - 1;
            if (edge || height[cell] <= 0.0F) {
                seen[cell] = true;
                open.push({.height = height[cell], .cell = static_cast<u32>(cell), .order = order++});
            }
        }
    }
    static constexpr std::array<std::array<i64, 2>, 8> kWays = {
        {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};
    while (!open.empty()) {
        const Open here = open.top();
        open.pop();
        visits.push_back(here.cell);
        const i64 cx = here.cell % kWindowCells;
        const i64 cz = here.cell / kWindowCells;
        for (const auto& way : kWays) {
            const i64 nx = cx + way[0];
            const i64 nz = cz + way[1];
            if (nx < 0 || nz < 0 || nx >= kWindowCells || nz >= kWindowCells) {
                continue;
            }
            const auto cell = static_cast<usize>((nz * kWindowCells) + nx);
            if (seen[cell]) {
                continue;
            }
            seen[cell] = true;
            filled[cell] = std::max(height[cell], here.height + kFlood);
            next[cell] = here.cell;
            open.push({.height = filled[cell], .cell = static_cast<u32>(cell), .order = order++});
        }
    }
    // Highest first, every cell passes its rain on to the one it drains into.
    std::vector<u32> rain(count, 1);
    for (auto it = visits.rbegin(); it != visits.rend(); ++it) {
        if (next[*it] != kNoNext && filled[*it] > 0.0F) {
            rain[next[*it]] += rain[*it];
        }
    }

    auto out = std::make_shared<Tile>();
    out->lakes.assign(static_cast<usize>(kTileCells * kTileCells), kNoLake);
    out->buckets.resize(static_cast<usize>(kBuckets * kBuckets));
    const i64 originX = key.x * kTileSize;
    const i64 originZ = key.z * kTileSize;
    const auto index = [&](i64 x, i64 z) { return static_cast<usize>((z * kWindowCells) + x); };
    for (i64 z = 0; z < kTileCells; ++z) {
        for (i64 x = 0; x < kTileCells; ++x) {
            const i64 wx = x + kTileCells;
            const i64 wz = z + kTileCells;
            const usize cell = index(wx, wz);
            if (filled[cell] <= 0.0F) {
                continue; // The sea.
            }
            const bool lake = filled[cell] - height[cell] >= kLakeDepth;
            if (lake) {
                out->lakes[static_cast<usize>((z * kTileCells) + x)] = std::floor(filled[cell]);
            }
            if (rain[cell] < kFirstStream || next[cell] == kNoNext) {
                continue;
            }
            const usize below = next[cell];
            if (lake && filled[below] - height[below] >= kLakeDepth) {
                continue; // Under the lake itself.
            }
            const i64 belowX = static_cast<i64>(below) % kWindowCells;
            const i64 belowZ = static_cast<i64>(below) / kWindowCells;
            const Spot a = spotOf((key.x * kTileCells) + x, (key.z * kTileCells) + z);
            const Spot b = spotOf((key.x * kTileCells) + belowX - kTileCells,
                                  (key.z * kTileCells) + belowZ - kTileCells);
            // Well below the planned ground, and never rising, since the filled
            // surface only falls along the flow. The plan is rougher than the
            // real ground, so this is what keeps water inside a trench it cut,
            // instead of standing above some lower spot beside it.
            const Stretch stretch{.ax = a.x,
                                  .az = a.z,
                                  .bx = b.x,
                                  .bz = b.z,
                                  .levelA = riverLevel(filled[cell]),
                                  .levelB = riverLevel(filled[below]),
                                  .widthA = widthOf(rain[cell]),
                                  .widthB = widthOf(rain[below])};
            const auto number = static_cast<u32>(out->stretches.size());
            out->stretches.push_back(stretch);
            const f64 reach = kBankReach + 1.0;
            const i64 firstX = floorDiv(static_cast<i64>(std::min(a.x, b.x) - reach) - originX, kBucket);
            const i64 lastX = floorDiv(static_cast<i64>(std::max(a.x, b.x) + reach) - originX, kBucket);
            const i64 firstZ = floorDiv(static_cast<i64>(std::min(a.z, b.z) - reach) - originZ, kBucket);
            const i64 lastZ = floorDiv(static_cast<i64>(std::max(a.z, b.z) + reach) - originZ, kBucket);
            for (i64 bz = std::max<i64>(firstZ, 0); bz <= std::min(lastZ, kBuckets - 1); ++bz) {
                for (i64 bx = std::max<i64>(firstX, 0); bx <= std::min(lastX, kBuckets - 1); ++bx) {
                    out->buckets[static_cast<usize>((bz * kBuckets) + bx)].push_back(number);
                }
            }
        }
    }
    return out;
}

void RiverNetwork::gather(f64 minX, f64 minZ, f64 maxX, f64 maxZ, std::vector<Stretch>& out) const {
    const auto toBlock = [](f64 value) { return static_cast<i64>(std::floor(value)); };
    const i64 firstX = floorDiv(toBlock(minX), kTileSize);
    const i64 lastX = floorDiv(toBlock(maxX), kTileSize);
    const i64 firstZ = floorDiv(toBlock(minZ), kTileSize);
    const i64 lastZ = floorDiv(toBlock(maxZ), kTileSize);
    // Stretches reach into the tiles beside theirs by their banks.
    for (i64 tz = firstZ - 1; tz <= lastZ + 1; ++tz) {
        for (i64 tx = firstX - 1; tx <= lastX + 1; ++tx) {
            const bool inside = tx >= firstX && tx <= lastX && tz >= firstZ && tz <= lastZ;
            const i64 originX = tx * kTileSize;
            const i64 originZ = tz * kTileSize;
            const bool close = toBlock(minX) - (originX + kTileSize) < 200 && originX - toBlock(maxX) < 200 &&
                               toBlock(minZ) - (originZ + kTileSize) < 200 && originZ - toBlock(maxZ) < 200;
            if (!inside && !close) {
                continue;
            }
            const auto part = tile({.x = tx, .z = tz});
            const i64 bx0 = std::clamp<i64>(floorDiv(toBlock(minX) - originX, kBucket), 0, kBuckets - 1);
            const i64 bx1 = std::clamp<i64>(floorDiv(toBlock(maxX) - originX, kBucket), 0, kBuckets - 1);
            const i64 bz0 = std::clamp<i64>(floorDiv(toBlock(minZ) - originZ, kBucket), 0, kBuckets - 1);
            const i64 bz1 = std::clamp<i64>(floorDiv(toBlock(maxZ) - originZ, kBucket), 0, kBuckets - 1);
            const usize before = out.size();
            for (i64 bz = bz0; bz <= bz1; ++bz) {
                for (i64 bx = bx0; bx <= bx1; ++bx) {
                    for (const u32 number : part->buckets[static_cast<usize>((bz * kBuckets) + bx)]) {
                        const Stretch& stretch = part->stretches[number];
                        // A box can span buckets: each stretch only once.
                        const bool known =
                            (bz1 > bz0 || bx1 > bx0) &&
                            std::ranges::any_of(out.begin() + static_cast<std::ptrdiff_t>(before), out.end(),
                                                [&](const Stretch& other) {
                                                    return other.ax == stretch.ax && other.az == stretch.az;
                                                });
                        if (!known) {
                            out.push_back(stretch);
                        }
                    }
                }
            }
        }
    }
}

RiverNetwork::Carve RiverNetwork::carve(std::span<const Stretch> stretches, f64 x, f64 z,
                                        f64 ground) noexcept {
    Carve out{.ground = ground};
    for (const Stretch& s : stretches) {
        const f64 abX = s.bx - s.ax;
        const f64 abZ = s.bz - s.az;
        const f64 length = (abX * abX) + (abZ * abZ);
        const f64 along =
            length > 0.0 ? std::clamp((((x - s.ax) * abX) + ((z - s.az) * abZ)) / length, 0.0, 1.0) : 0.0;
        const f64 gapX = x - (s.ax + (abX * along));
        const f64 gapZ = z - (s.az + (abZ * along));
        const f64 distance = std::sqrt((gapX * gapX) + (gapZ * gapZ));
        const f64 half = (s.widthA + ((s.widthB - s.widthA) * along)) * 0.5;
        if (distance >= half + kBankReach) {
            continue;
        }
        const f64 level = std::floor(s.levelA + ((s.levelB - s.levelA) * along));
        if (distance < half) {
            // The channel: cut down into the rock, deeper in wide water.
            const f64 side = distance / half;
            const f64 depth = 1.0 + std::min(half * 0.55, 5.0);
            out.ground = std::min(out.ground, level - (depth * (1.0 - (side * side))));
            out.water = std::max(out.water, level);
        } else {
            // The banks: the ground leans back from the water, as a valley
            // does. Only ever cut, never raised: water makes its own way.
            const f64 away = distance - half;
            out.ground = std::min(out.ground, level + 1.0 + (away * away * 0.045) + (away * 0.3));
        }
    }
    return out;
}

f64 RiverNetwork::lakeAt(f64 x, f64 z) const {
    const auto bx = static_cast<i64>(std::floor(x));
    const auto bz = static_cast<i64>(std::floor(z));
    const TileKey key{.x = floorDiv(bx, kTileSize), .z = floorDiv(bz, kTileSize)};
    const auto part = tile(key);
    const i64 cellX = floorDiv(bx - (key.x * kTileSize), kCell);
    const i64 cellZ = floorDiv(bz - (key.z * kTileSize), kCell);
    const f32 level = part->lakes[static_cast<usize>((cellZ * kTileCells) + cellX)];
    return level > kNoLake ? static_cast<f64>(level) : kNoWater;
}

} // namespace vr
