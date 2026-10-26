#pragma once

#include <vxe/core/types.hpp>
#include <vxe/math/aabb.hpp>
#include <vxe/math/vector.hpp>

#include <array>

namespace vxe {

// The six planes of a Frustum.
enum class FrustumPlane : u8 {
    Left,
    Right,
    Bottom,
    Top,
    Near,
    Far
};

inline constexpr usize kFrustumPlaneCount = 6;

// A plane as (normal, distance): points p with dot(normal, p) + distance >= 0
// are on the inside. The normal is unit length, so the value is the signed
// distance to the plane in world units.
struct Plane {
    Vec3 normal{0.0F, 0.0F, 1.0F};
    float distance = 0.0F;

    [[nodiscard]] float signedDistance(const Vec3& point) const noexcept {
        return glm::dot(normal, point) + distance;
    }
};

// The view volume of a camera as six planes, for culling.
//
// The planes are extracted straight from a view projection matrix
// (Gribb and Hartmann): each clip space inequality such as -w <= x is a plane
// in world space formed by adding or subtracting rows of the matrix. This
// works for any projection, including the reversed Z, infinite far plane
// projection the renderer uses (ADR-010).
//
// With an infinite far plane, the "far" inequality has no real plane: its row
// combination is (0, 0, 0, near), which every point satisfies. Normalizing it
// would divide by zero, so such a degenerate plane is replaced by one that
// accepts everything. The test results stay correct.
class Frustum {
public:
    Frustum() = default;

    // `viewProjection` maps world (or camera relative) positions to clip space
    // with depth from 0 to 1 (GLM_FORCE_DEPTH_ZERO_TO_ONE). Works for normal
    // and reversed Z.
    [[nodiscard]] static Frustum fromViewProjection(const Mat4& viewProjection) noexcept;

    [[nodiscard]] const Plane& plane(FrustumPlane which) const noexcept {
        return m_planes[static_cast<usize>(which)];
    }

    // True if the point is inside every plane.
    [[nodiscard]] bool contains(const Vec3& point) const noexcept;

    // False only if the box is certainly outside. Uses the positive vertex
    // test: for each plane, only the box corner furthest along the plane
    // normal needs checking, which is one dot product per plane. Boxes near
    // frustum corners may be reported visible though they are not, which
    // only costs a little extra drawing.
    [[nodiscard]] bool intersects(const Aabb& box) const noexcept;

private:
    void setPlane(FrustumPlane which, const Plane& plane) noexcept {
        m_planes[static_cast<usize>(which)] = plane;
    }

    std::array<Plane, kFrustumPlaneCount> m_planes{};
};

} // namespace vxe
