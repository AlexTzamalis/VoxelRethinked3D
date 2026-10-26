#pragma once

#include <vxe/core/assert.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/coords.hpp>
#include <vxe/math/face.hpp>
#include <vxe/math/vector.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <limits>
#include <optional>

namespace vxe {

// The result of a ray that hit a solid block.
struct RaycastHit {
    BlockPos block;     // The solid block that was hit.
    Face face;          // The face of that block the ray entered through. A
                        // block placed against the hit goes to block + normalOf(face).
    f64 distance = 0.0; // Distance from the ray origin to the hit, in blocks.
    DVec3 point{0.0};   // Where the ray touched the block.
};

// Something that answers "is this block solid for the ray?". A template
// parameter instead of std::function keeps the per block call inlinable
// (Section 23.3: no std::function in hot paths).
template <typename Fn>
concept BlockPredicate = std::predicate<Fn&, const BlockPos&>;

// Walks the blocks a ray passes through, in order, until `isSolid` returns
// true or `maxDistance` is reached (Amanatides and Woo, "A Fast Voxel
// Traversal Algorithm for Ray Tracing", 1987).
//
// Instead of stepping the ray by small increments (which can skip corners and
// costs many samples), the algorithm jumps from one block boundary to the
// next: for each axis it tracks the ray distance to the next boundary on that
// axis (tMax) and how far the ray travels between two boundaries (tDelta).
// The smallest tMax decides which neighbor comes next. Every block the ray
// touches is visited exactly once.
//
// All math is in double, so targeting works the same 30 million blocks from
// the origin as next to it (ADR-011).
//
// If the origin is already inside a solid block, that block is returned at
// distance 0, with the face pointing back against the ray's main direction.
// A zero direction returns nothing.
template <BlockPredicate IsSolid>
[[nodiscard]] std::optional<RaycastHit> raycastBlocks(const DVec3& origin, const DVec3& direction,
                                                      f64 maxDistance, IsSolid&& isSolid) {
    VXE_ASSERT(std::isfinite(maxDistance) && maxDistance >= 0.0, "maxDistance must be finite");
    const f64 length = glm::length(direction);
    if (!(length > 0.0) || !std::isfinite(length)) {
        return std::nullopt;
    }
    const DVec3 dir = direction / length;

    BlockPos block = blockAt(origin);
    std::array<i64*, 3> cell{&block.x, &block.y, &block.z};

    std::array<int, 3> step{};
    std::array<f64, 3> tMax{};
    std::array<f64, 3> tDelta{};
    constexpr f64 kInf = std::numeric_limits<f64>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        const f64 d = dir[axis];
        // The offset inside the starting block (0 to 1), computed from the
        // block index so it stays exact even far from the origin.
        const f64 inside = origin[axis] - static_cast<f64>(*cell[axis]);
        if (d > 0.0) {
            step[axis] = 1;
            tDelta[axis] = 1.0 / d;
            tMax[axis] = (1.0 - inside) / d;
        } else if (d < 0.0) {
            step[axis] = -1;
            tDelta[axis] = -1.0 / d;
            tMax[axis] = inside / -d;
        } else {
            step[axis] = 0;
            tDelta[axis] = kInf;
            tMax[axis] = kInf;
        }
    }

    if (isSolid(static_cast<const BlockPos&>(block))) {
        // Started inside a block: report it with the face the ray "comes from".
        int mainAxis = 0;
        for (int axis = 1; axis < 3; ++axis) {
            if (std::abs(dir[axis]) > std::abs(dir[mainAxis])) {
                mainAxis = axis;
            }
        }
        const int sign = dir[mainAxis] > 0.0 ? -1 : 1;
        return RaycastHit{
            .block = block, .face = faceFromAxis(mainAxis, sign), .distance = 0.0, .point = origin};
    }

    // Each step crosses one boundary, and a ray of length L crosses at most
    // about L boundaries per axis. The cap only guards against bad input.
    const auto maxSteps = static_cast<i64>(std::ceil(maxDistance) * 3.0) + 3;
    for (i64 i = 0; i < maxSteps; ++i) {
        int axis = 0;
        if (tMax[1] < tMax[axis]) {
            axis = 1;
        }
        if (tMax[2] < tMax[axis]) {
            axis = 2;
        }
        const f64 distance = tMax[axis];
        if (distance > maxDistance) {
            return std::nullopt;
        }
        *cell[axis] += step[axis];
        tMax[axis] += tDelta[axis];

        if (isSolid(static_cast<const BlockPos&>(block))) {
            // Moving in +X enters the new block through its -X face, and so on.
            return RaycastHit{.block = block,
                              .face = faceFromAxis(axis, -step[axis]),
                              .distance = distance,
                              .point = origin + dir * distance};
        }
    }
    return std::nullopt;
}

} // namespace vxe
