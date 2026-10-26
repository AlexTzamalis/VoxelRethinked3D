#include "../test_helpers.hpp"

#include <vxe/math/frustum.hpp>
#include <vxe/math/projection.hpp>

#include <doctest/doctest.h>

#include <numbers>

using vxe::BlockPos;
using vxe::CameraPosition;
using vxe::ChunkPos;
using vxe::DVec3;
using vxe::Mat4;
using vxe::Vec3;
using vxe::Vec4;

namespace {

constexpr float kFov = std::numbers::pi_v<float> / 2.0F;
constexpr float kNear = 0.1F;

// Projects a view space point and returns its depth after the perspective divide.
float depthOf(const Mat4& projection, float viewZ) {
    const Vec4 clip = projection * Vec4(0.0F, 0.0F, viewZ, 1.0F);
    return clip.z / clip.w;
}

} // namespace

TEST_CASE("Reversed Z maps near to 1 and infinity towards 0") {
    const Mat4 projection = vxe::perspectiveReversedInfinite(kFov, 16.0F / 9.0F, kNear);

    CHECK(depthOf(projection, -kNear) == doctest::Approx(1.0F));
    CHECK(depthOf(projection, -2.0F * kNear) == doctest::Approx(0.5F));
    CHECK(depthOf(projection, -1000.0F) == doctest::Approx(kNear / 1000.0F));

    const float veryFar = depthOf(projection, -1e30F);
    CHECK(veryFar > 0.0F);
    CHECK(veryFar < 1e-29F);

    // Farther is always smaller: a GREATER depth test keeps the nearest surface.
    CHECK(depthOf(projection, -32'000.0F) < depthOf(projection, -31'999.0F));
}

TEST_CASE("Reversed Z keeps distant surfaces apart") {
    // Two surfaces one block apart at the far LOD distance (32,000 blocks)
    // must still get different 32 bit depth values.
    const Mat4 projection = vxe::perspectiveReversedInfinite(kFov, 1.0F, kNear);
    CHECK(depthOf(projection, -32'000.0F) != depthOf(projection, -32'001.0F));
}

TEST_CASE("Camera relative view is a pure rotation") {
    const Mat4 view = vxe::cameraRelativeView(Vec3(0.0F, 0.0F, -1.0F));
    // The camera sits at the origin, so the translation column is zero.
    CHECK(view[3] == Vec4(0.0F, 0.0F, 0.0F, 1.0F));

    const Mat4 viewProjection = vxe::perspectiveReversedInfinite(kFov, 1.0F, kNear) * view;
    const vxe::Frustum frustum = vxe::Frustum::fromViewProjection(viewProjection);
    CHECK(frustum.contains(Vec3(0.0F, 0.0F, -100.0F)));
    CHECK_FALSE(frustum.contains(Vec3(0.0F, 0.0F, 100.0F)));
}

TEST_CASE("CameraPosition splits world positions exactly") {
    const CameraPosition near = CameraPosition::fromWorld(DVec3(17.25, -0.5, 3.0));
    CHECK(near.chunk == ChunkPos{.x = 1, .y = -1, .z = 0});
    CHECK(near.fraction == Vec3(1.25F, 15.5F, 3.0F));

    // 10 million blocks out a float alone has a step of 1 block; the split
    // keeps the quarter block offset exact.
    const CameraPosition far = CameraPosition::fromWorld(DVec3(10'000'000.25, 64.75, -10'000'000.5));
    // Block -10,000,001 is in chunk -625,001: chunk -625,000 starts exactly at -10,000,000.
    CHECK(far.chunk == ChunkPos{.x = 625'000, .y = 4, .z = -625'001});
    CHECK(far.fraction == Vec3(0.25F, 0.75F, 15.5F));
}

TEST_CASE("Positions relative to the camera stay exact far from the origin") {
    const CameraPosition camera = CameraPosition::fromWorld(DVec3(10'000'000.25, 64.0, 10'000'000.0));

    // A block 3 blocks east of the camera's block.
    const Vec3 block = vxe::relativeToCamera(BlockPos{.x = 10'000'003, .y = 64, .z = 10'000'000}, camera);
    CHECK(block == Vec3(2.75F, 0.0F, 0.0F));

    // The chunk next to the camera chunk on +X.
    const ChunkPos next = camera.chunk.offset(1, 0, 0);
    const Vec3 chunk = vxe::relativeToCamera(next, camera);
    CHECK(chunk == Vec3(16.0F - 0.25F, 0.0F, 0.0F));

    // Chunks at opposite ends of the grid do not overflow the integer math.
    constexpr ChunkPos kLowest{.x = INT32_MIN, .y = 0, .z = 0};
    const CameraPosition highest{.chunk = ChunkPos{.x = INT32_MAX, .y = 0, .z = 0}, .fraction = Vec3(0.0F)};
    CHECK(vxe::relativeToCamera(kLowest, highest).x < -6.8e10F);
}
