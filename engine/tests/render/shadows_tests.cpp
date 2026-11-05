#include <vxe/render/shadows.hpp>

#include <doctest/doctest.h>

#include <cmath>

using namespace vxe;
using namespace vxe::render;

namespace {

// Where a world point lands in a cascade, in texels of its tile, for a camera.
Vec3 texelOf(const ShadowCascade& cascade, const Camera& camera, const DVec3& world) {
    const Vec4 clip = cascade.matrix * Vec4(Vec3(world - camera.position), 1.0F);
    return {((clip.x * 0.5F) + 0.5F) * static_cast<f32>(kShadowTileSize),
            ((clip.y * 0.5F) + 0.5F) * static_cast<f32>(kShadowTileSize), clip.z};
}

} // namespace

TEST_CASE("Shadow cascades reach further one after another and cover the ground in view") {
    Camera camera;
    camera.position = {1000.5, 80.0, -2000.25};
    const Vec3 toSun = glm::normalize(Vec3(0.3F, 0.9F, 0.2F));
    const ShadowFrame frame = computeShadowFrame(camera, 16.0F / 9.0F, toSun, 1.0F, 4, 128.0F);
    REQUIRE(frame.enabled);
    REQUIRE(frame.cascades == 4);
    for (u32 i = 1; i < frame.cascades; ++i) {
        CHECK(frame.cascade.at(i).far > frame.cascade.at(i - 1).far);
        CHECK(frame.cascade.at(i).radius >= frame.cascade.at(i - 1).radius);
    }
    CHECK(frame.cascade.back().far == doctest::Approx(128.0F));
    // A point on the ground ahead lands inside the first cascades' tiles, and
    // a point nearer the sun gets a larger depth (reversed Z).
    const DVec3 ahead = camera.position + DVec3(camera.forward()) * 5.0;
    const Vec3 onMap = texelOf(frame.cascade.at(0), camera, ahead);
    CHECK(onMap.x > 0.0F);
    CHECK(onMap.x < static_cast<f32>(kShadowTileSize));
    CHECK(onMap.y > 0.0F);
    CHECK(onMap.y < static_cast<f32>(kShadowTileSize));
    CHECK(onMap.z > 0.0F);
    CHECK(onMap.z < 1.0F);
    const Vec3 above = texelOf(frame.cascade.at(0), camera, ahead + DVec3(toSun) * 4.0);
    CHECK(above.z > onMap.z);
}

TEST_CASE("Shadow edges stay on the same texels while the camera moves a little") {
    Camera camera;
    camera.position = {123456.25, 90.0, -654321.5};
    const Vec3 toSun = glm::normalize(Vec3(-0.4F, 0.8F, 0.3F));
    const ShadowFrame before = computeShadowFrame(camera, 1.5F, toSun, 1.0F, 3, 128.0F);
    Camera moved = camera;
    moved.position += DVec3(0.37, 0.0, -0.21);
    const ShadowFrame after = computeShadowFrame(moved, 1.5F, toSun, 1.0F, 3, 128.0F);
    const DVec3 corner = camera.position + DVec3(4.0, -2.0, -6.0);
    for (u32 i = 0; i < before.cascades; ++i) {
        const Vec3 a = texelOf(before.cascade.at(i), camera, corner);
        const Vec3 b = texelOf(after.cascade.at(i), moved, corner);
        // Whole texels apart: the same point falls on the same spot of a texel.
        CHECK(std::abs((a.x - std::floor(a.x)) - (b.x - std::floor(b.x))) < 0.01F);
        CHECK(std::abs((a.y - std::floor(a.y)) - (b.y - std::floor(b.y))) < 0.01F);
    }
}

TEST_CASE("The sun casts by day, the moon faintly by night, and neither at the horizon") {
    const Vec3 down(0.0F, -1.0F, 0.0F);
    const ShadowLight noon = shadowLightOf(Vec3(0.0F, 1.0F, 0.0F), down);
    CHECK(noon.strength == doctest::Approx(1.0F));
    CHECK(noon.toLight.y == doctest::Approx(1.0F));
    const ShadowLight sunset = shadowLightOf(glm::normalize(Vec3(1.0F, 0.01F, 0.0F)), down);
    CHECK(sunset.strength == doctest::Approx(0.0F));
    const ShadowLight night = shadowLightOf(down, Vec3(0.0F, 1.0F, 0.0F));
    CHECK(night.strength == doctest::Approx(0.5F));
    CHECK(night.toLight.y == doctest::Approx(1.0F));
}
