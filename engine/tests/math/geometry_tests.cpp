#include <vxe/core/containers.hpp>
#include <vxe/math/aabb.hpp>
#include <vxe/math/face.hpp>
#include <vxe/math/frustum.hpp>
#include <vxe/math/raycast.hpp>

#include <doctest/doctest.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using vxe::Aabb;
using vxe::BlockPos;
using vxe::DVec3;
using vxe::Face;
using vxe::Frustum;
using vxe::FrustumPlane;
using vxe::Mat4;
using vxe::Vec3;

namespace {

// Reversed Z, infinite far plane, depth 0 to 1, camera looking down -Z
// (ADR-010). Built by hand here; E2.3 adds the engine helper.
Mat4 reversedInfinitePerspective(float fovY, float aspect, float zNear) {
    const float f = 1.0F / std::tan(fovY * 0.5F);
    Mat4 m(0.0F);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[3][2] = zNear; // z_clip = near, so depth = near / -z_view.
    m[2][3] = -1.0F; // w_clip = -z_view.
    return m;
}

constexpr float kFov = std::numbers::pi_v<float> / 2.0F; // 90 degrees.

// A camera at the origin looking down -Z.
Mat4 viewLookingDownNegativeZ() {
    return glm::lookAt(Vec3(0.0F), Vec3(0.0F, 0.0F, -1.0F), Vec3(0.0F, 1.0F, 0.0F));
}

} // namespace

TEST_CASE("Face helpers") {
    static_assert(vxe::opposite(Face::PosX) == Face::NegX);
    static_assert(vxe::opposite(Face::NegZ) == Face::PosZ);
    static_assert(vxe::axisOf(Face::PosY) == 1);
    static_assert(vxe::signOf(Face::NegY) == -1);
    static_assert(vxe::faceFromAxis(2, 1) == Face::PosZ);
    CHECK(vxe::normalOf(Face::NegX) == vxe::IVec3(-1, 0, 0));
    CHECK(vxe::nameOf(Face::PosZ) == "PosZ");
    for (const Face face : vxe::kAllFaces) {
        CHECK(vxe::faceFromAxis(vxe::axisOf(face), vxe::signOf(face)) == face);
        CHECK(vxe::normalOf(face) == -vxe::normalOf(vxe::opposite(face)));
    }
}

TEST_CASE("Aabb basics") {
    Aabb box = Aabb::empty();
    CHECK(box.isEmpty());
    box.expand(Vec3(1.0F, 2.0F, 3.0F));
    box.expand(Vec3(-1.0F, 0.0F, 5.0F));
    CHECK_FALSE(box.isEmpty());
    CHECK(box.min == Vec3(-1.0F, 0.0F, 3.0F));
    CHECK(box.max == Vec3(1.0F, 2.0F, 5.0F));
    CHECK(box.center() == Vec3(0.0F, 1.0F, 4.0F));
    CHECK(box.contains(Vec3(0.0F, 1.0F, 4.0F)));
    CHECK_FALSE(box.contains(Vec3(0.0F, 3.0F, 4.0F)));

    const Aabb other = Aabb::fromCenterExtents(Vec3(2.0F, 1.0F, 4.0F), Vec3(1.0F));
    CHECK(box.intersects(other)); // Touching at x = 1 counts.
    CHECK_FALSE(box.intersects(other.translated(Vec3(0.5F, 0.0F, 0.0F))));
    CHECK(box.inflated(1.0F).contains(Vec3(-2.0F, 0.0F, 4.0F)));
}

TEST_CASE("Frustum from a standard perspective with depth 0 to 1") {
    const Mat4 projection = glm::perspective(kFov, 1.0F, 0.1F, 100.0F);
    const Frustum frustum = Frustum::fromViewProjection(projection * viewLookingDownNegativeZ());

    CHECK(frustum.contains(Vec3(0.0F, 0.0F, -10.0F)));
    CHECK_FALSE(frustum.contains(Vec3(0.0F, 0.0F, 10.0F)));    // Behind.
    CHECK_FALSE(frustum.contains(Vec3(0.0F, 0.0F, -200.0F)));  // Past the far plane.
    CHECK_FALSE(frustum.contains(Vec3(-20.0F, 0.0F, -10.0F))); // Left of a 90 degree view.
    CHECK(frustum.contains(Vec3(-9.0F, 0.0F, -10.0F)));

    // The left plane passes through the origin, so its signed distance is a
    // real distance in world units.
    const vxe::Plane& left = frustum.plane(FrustumPlane::Left);
    CHECK(std::abs(glm::length(left.normal) - 1.0F) < 1e-5F);
    CHECK(left.signedDistance(Vec3(0.0F)) == doctest::Approx(0.0F).epsilon(1e-5));
}

TEST_CASE("Frustum from a reversed Z infinite projection") {
    const Frustum frustum = Frustum::fromViewProjection(
        reversedInfinitePerspective(kFov, 16.0F / 9.0F, 0.1F) * viewLookingDownNegativeZ());

    CHECK(frustum.contains(Vec3(0.0F, 0.0F, -1.0F)));
    CHECK(frustum.contains(Vec3(0.0F, 0.0F, -32'000.0F))); // Far LOD distances are never culled by depth.
    CHECK_FALSE(frustum.contains(Vec3(0.0F, 0.0F, 1.0F)));
    CHECK_FALSE(frustum.contains(Vec3(0.0F, 0.0F, -0.05F))); // Closer than the near plane.

    // The far "plane" is degenerate and accepts every point instead of being NaN.
    const vxe::Plane& far = frustum.plane(FrustumPlane::Far);
    CHECK(far.normal == Vec3(0.0F));
    CHECK(far.signedDistance(Vec3(1e9F)) > 0.0F);
}

TEST_CASE("Frustum box test uses the positive vertex") {
    const Frustum frustum = Frustum::fromViewProjection(reversedInfinitePerspective(kFov, 1.0F, 0.1F) *
                                                        viewLookingDownNegativeZ());

    CHECK(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0F, 0.0F, -20.0F), Vec3(1.0F))));
    CHECK(frustum.intersects(
        Aabb::fromCenterExtents(Vec3(-20.0F, 0.0F, -20.0F), Vec3(1.0F)))); // On the left edge.
    CHECK_FALSE(frustum.intersects(Aabb::fromCenterExtents(Vec3(-40.0F, 0.0F, -20.0F), Vec3(1.0F))));
    CHECK_FALSE(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0F, 0.0F, 20.0F), Vec3(1.0F))));
    // A chunk sized box around the camera is always visible.
    CHECK(frustum.intersects(Aabb::fromCenterExtents(Vec3(0.0F), Vec3(8.0F))));
}

namespace {

// A world described by a set of solid blocks, for raycast tests.
struct TestWorld {
    vxe::HashSet<BlockPos> solid;
    bool operator()(const BlockPos& block) const { return solid.contains(block); }
};

} // namespace

TEST_CASE("Raycast along an axis hits the entry face") {
    const TestWorld world{.solid = {BlockPos{.x = 5, .y = 0, .z = 0}}};
    const auto hit = vxe::raycastBlocks(DVec3(0.5, 0.5, 0.5), DVec3(1.0, 0.0, 0.0), 10.0, world);
    REQUIRE(hit.has_value());
    CHECK(hit->block == BlockPos{.x = 5, .y = 0, .z = 0});
    CHECK(hit->face == Face::NegX);
    CHECK(hit->distance == doctest::Approx(4.5));
    CHECK(hit->point.x == doctest::Approx(5.0));

    // Placing a block against that face puts it back towards the ray origin.
    CHECK(hit->block + vxe::normalOf(hit->face) == BlockPos{.x = 4, .y = 0, .z = 0});
}

TEST_CASE("Raycast in negative directions and through negative coordinates") {
    const TestWorld world{.solid = {BlockPos{.x = 0, .y = -3, .z = 0}}};
    const auto hit = vxe::raycastBlocks(DVec3(0.5, 2.25, 0.5), DVec3(0.0, -1.0, 0.0), 10.0, world);
    REQUIRE(hit.has_value());
    CHECK(hit->block == BlockPos{.x = 0, .y = -3, .z = 0});
    CHECK(hit->face == Face::PosY);
    CHECK(hit->distance == doctest::Approx(4.25));
}

TEST_CASE("Raycast visits every block a diagonal ray touches, in order") {
    std::vector<BlockPos> visited;
    auto record = [&visited](const BlockPos& block) {
        visited.push_back(block);
        return false;
    };
    const auto hit = vxe::raycastBlocks(DVec3(0.5, 0.25, 0.5), DVec3(1.0, 1.0, 0.0), 3.0, record);
    CHECK_FALSE(hit.has_value());
    REQUIRE(visited.size() >= 4);
    CHECK(visited[0] == BlockPos{.x = 0, .y = 0, .z = 0});
    // Each step moves to a face neighbor, never diagonally through a corner.
    for (std::size_t i = 1; i < visited.size(); ++i) {
        const vxe::i64 dx = std::abs(visited[i].x - visited[i - 1].x);
        const vxe::i64 dy = std::abs(visited[i].y - visited[i - 1].y);
        const vxe::i64 dz = std::abs(visited[i].z - visited[i - 1].z);
        CHECK(dx + dy + dz == 1);
    }
}

TEST_CASE("Raycast misses beyond the maximum distance") {
    const TestWorld world{.solid = {BlockPos{.x = 0, .y = 0, .z = -20}}};
    CHECK_FALSE(vxe::raycastBlocks(DVec3(0.5), DVec3(0.0, 0.0, -1.0), 8.0, world).has_value());
    CHECK(vxe::raycastBlocks(DVec3(0.5), DVec3(0.0, 0.0, -1.0), 20.0, world).has_value());
}

TEST_CASE("Raycast starting inside a solid block reports it at distance zero") {
    const TestWorld world{.solid = {BlockPos{}}};
    const auto hit = vxe::raycastBlocks(DVec3(0.5), DVec3(0.0, 0.0, 1.0), 5.0, world);
    REQUIRE(hit.has_value());
    CHECK(hit->block == BlockPos{});
    CHECK(hit->distance == 0.0);
    CHECK(hit->face == Face::NegZ);
}

TEST_CASE("Raycast is exact far from the origin") {
    constexpr double kFar = 30'000'000.0;
    const TestWorld world{.solid = {BlockPos{.x = 30'000'003, .y = 64, .z = -30'000'000}}};
    const auto hit =
        vxe::raycastBlocks(DVec3(kFar + 0.5, 64.5, -kFar + 0.5), DVec3(1.0, 0.0, 0.0), 10.0, world);
    REQUIRE(hit.has_value());
    CHECK(hit->block == BlockPos{.x = 30'000'003, .y = 64, .z = -30'000'000});
    CHECK(hit->distance == doctest::Approx(2.5));
}

TEST_CASE("Raycast with a zero direction hits nothing") {
    const TestWorld world{.solid = {BlockPos{}}};
    CHECK_FALSE(vxe::raycastBlocks(DVec3(0.5), DVec3(0.0), 5.0, world).has_value());
}
