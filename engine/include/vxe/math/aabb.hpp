#pragma once

#include <vxe/math/vector.hpp>

#include <glm/common.hpp>

#include <limits>

namespace vxe {

// An axis aligned bounding box in single precision.
//
// Float is enough because boxes used for culling and collision are always
// relative to something nearby (the camera chunk, ADR-011), never absolute
// world positions millions of blocks from the origin.
struct Aabb {
    Vec3 min{0.0F};
    Vec3 max{0.0F};

    // A box that contains nothing. Expanding it by a point gives a box of
    // exactly that point, which makes "bounds of these points" loops simple.
    [[nodiscard]] static Aabb empty() noexcept {
        constexpr float kInf = std::numeric_limits<float>::infinity();
        return {.min = Vec3(kInf), .max = Vec3(-kInf)};
    }

    [[nodiscard]] static Aabb fromCenterExtents(const Vec3& center, const Vec3& halfExtents) noexcept {
        return {.min = center - halfExtents, .max = center + halfExtents};
    }

    [[nodiscard]] bool isEmpty() const noexcept { return min.x > max.x || min.y > max.y || min.z > max.z; }
    [[nodiscard]] Vec3 center() const noexcept { return (min + max) * 0.5F; }
    [[nodiscard]] Vec3 halfExtents() const noexcept { return (max - min) * 0.5F; }
    [[nodiscard]] Vec3 size() const noexcept { return max - min; }

    // True if the point is inside or on the boundary.
    [[nodiscard]] bool contains(const Vec3& point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    // True if the boxes overlap. Touching faces count as overlapping.
    [[nodiscard]] bool intersects(const Aabb& other) const noexcept {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }

    void expand(const Vec3& point) noexcept {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void expand(const Aabb& other) noexcept {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    [[nodiscard]] Aabb translated(const Vec3& offset) const noexcept {
        return {.min = min + offset, .max = max + offset};
    }

    // Grows the box by `amount` on every side (for example frustum padding).
    [[nodiscard]] Aabb inflated(float amount) const noexcept {
        return {.min = min - Vec3(amount), .max = max + Vec3(amount)};
    }
};

} // namespace vxe
