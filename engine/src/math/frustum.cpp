#include "vxe/math/frustum.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>

namespace vxe {
namespace {

// Turns a raw (a, b, c, d) row combination into a normalized plane, or into a
// plane that accepts every point when the combination has no direction.
Plane makePlane(const Vec4& raw) noexcept {
    const Vec3 normal(raw);
    const float length = glm::length(normal);
    constexpr float kDegenerate = 1e-6F;
    if (length < kDegenerate) {
        // No direction means no restriction (the infinite far plane). A zero
        // normal with distance 1 gives signedDistance 1 for every point.
        return {.normal = Vec3(0.0F), .distance = 1.0F};
    }
    return {.normal = normal / length, .distance = raw.w / length};
}

} // namespace

Frustum Frustum::fromViewProjection(const Mat4& viewProjection) noexcept {
    // glm is column major, so a row of the matrix is gathered across columns.
    const Mat4 m = glm::transpose(viewProjection);
    const Vec4 row0 = m[0];
    const Vec4 row1 = m[1];
    const Vec4 row2 = m[2];
    const Vec4 row3 = m[3];

    // Clip space keeps points with -w <= x <= w, -w <= y <= w and 0 <= z <= w.
    // With reversed Z, z = w is the near plane and z = 0 the far one; with
    // normal depth it is the other way round. Each inequality becomes a plane,
    // and which one is called near does not matter for culling.
    Frustum frustum;
    frustum.setPlane(FrustumPlane::Left, makePlane(row3 + row0));
    frustum.setPlane(FrustumPlane::Right, makePlane(row3 - row0));
    frustum.setPlane(FrustumPlane::Bottom, makePlane(row3 + row1));
    frustum.setPlane(FrustumPlane::Top, makePlane(row3 - row1));
    frustum.setPlane(FrustumPlane::Near, makePlane(row3 - row2)); // z <= w
    frustum.setPlane(FrustumPlane::Far, makePlane(row2));         // z >= 0
    return frustum;
}

bool Frustum::contains(const Vec3& point) const noexcept {
    return std::ranges::all_of(m_planes,
                               [&point](const Plane& plane) { return plane.signedDistance(point) >= 0.0F; });
}

bool Frustum::intersects(const Aabb& box) const noexcept {
    return std::ranges::all_of(m_planes, [&box](const Plane& plane) {
        // The corner of the box that lies furthest along the plane normal.
        const Vec3 positive(plane.normal.x >= 0.0F ? box.max.x : box.min.x,
                            plane.normal.y >= 0.0F ? box.max.y : box.min.y,
                            plane.normal.z >= 0.0F ? box.max.z : box.min.z);
        return plane.signedDistance(positive) >= 0.0F;
    });
}

} // namespace vxe
