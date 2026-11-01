#pragma once

#include "worldgen/planet.hpp"

#include <vxe/core/types.hpp>

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace vr {

// Rivers and lakes from real drainage (G3.5).
//
// Rain falls on the planet's actual heights. On a grid of 48 blocks every cell
// drains into a neighbor, found by flooding the land up from the sea (priority
// flood): that finds a way out of every hollow, and how high a hollow fills
// before it spills, which is a lake. Counting the cells that drain through
// each cell gives its water: many thin streams in the mountains, joining into
// ever wider rivers towards the sea. Width comes from that count and nothing
// else, and the water level only ever falls along the way.
//
// The result comes in tiles of 8160 blocks. A tile looks at itself and the 8
// around it, about 24 km, which holds a whole default continent's basins, so
// that it is a pure function of the seed and the tile's coordinates: any order,
// any thread. A river longer than that could change width at a tile border.
//
// Water in a river stands still until flowing water exists; this class gives
// the direction it would flow in by the order of a stretch's ends.
//
// Thread safety: all const functions may run on any thread. Tiles are built
// once by whichever thread asks first, and others wait for it.
class RiverNetwork {
public:
    // One piece of a river, from upstream (a) to downstream (b). Levels are
    // above the sea, widths in blocks.
    struct Stretch {
        vxe::f64 ax = 0.0;
        vxe::f64 az = 0.0;
        vxe::f64 bx = 0.0;
        vxe::f64 bz = 0.0;
        vxe::f64 levelA = 0.0;
        vxe::f64 levelB = 0.0;
        vxe::f64 widthA = 1.0;
        vxe::f64 widthB = 1.0;
    };

    // What the rivers around a position do to it.
    struct Carve {
        vxe::f64 ground = 0.0;     // The ground after cutting; only ever lower.
        vxe::f64 water = kNoWater; // The water surface, if in a river.
        vxe::f64 damp = 0.0;       // 1 at the water, falling to 0 away from it.
    };

    // How far from a stretch its banks can reach, in blocks.
    static constexpr vxe::f64 kBankReach = 72.0;

    explicit RiverNetwork(const Planet& planet);
    ~RiverNetwork();
    RiverNetwork(const RiverNetwork&) = delete;
    RiverNetwork& operator=(const RiverNetwork&) = delete;
    RiverNetwork(RiverNetwork&&) = delete;
    RiverNetwork& operator=(RiverNetwork&&) = delete;

    // The stretches whose banks can reach into this box, added to `out`.
    void gather(vxe::f64 minX, vxe::f64 minZ, vxe::f64 maxX, vxe::f64 maxZ, std::vector<Stretch>& out) const;

    // Cuts the ground at a position with the stretches gathered for it.
    [[nodiscard]] static Carve carve(std::span<const Stretch> stretches, vxe::f64 x, vxe::f64 z,
                                     vxe::f64 ground) noexcept;

    // The surface of the lake over this position, above the sea, or kNoWater.
    [[nodiscard]] vxe::f64 lakeAt(vxe::f64 x, vxe::f64 z) const;

private:
    struct Tile;
    struct Heights;
    struct TileKey {
        vxe::i64 x = 0;
        vxe::i64 z = 0;
        bool operator==(const TileKey&) const = default;
    };
    struct TileKeyHash {
        std::size_t operator()(const TileKey& key) const noexcept;
    };
    template <typename Value>
    struct Slot {
        std::once_flag once;
        std::shared_ptr<const Value> value;
    };
    template <typename Value>
    struct Cache {
        std::mutex mutex;
        std::unordered_map<TileKey, std::shared_ptr<Slot<Value>>, TileKeyHash> slots;
        std::list<TileKey> order; // Oldest first.
    };

    [[nodiscard]] std::shared_ptr<const Heights> heights(TileKey key) const;
    [[nodiscard]] std::shared_ptr<const Tile> tile(TileKey key) const;
    [[nodiscard]] std::shared_ptr<const Tile> buildTile(TileKey key) const;
    [[nodiscard]] std::shared_ptr<const Heights> buildHeights(TileKey key) const;

    const Planet* m_planet;
    vxe::u64 m_id;
    mutable Cache<Heights> m_heights;
    mutable Cache<Tile> m_tiles;
};

} // namespace vr
