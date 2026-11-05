#include <vxe/render/god_rays.hpp>

#include <doctest/doctest.h>

using namespace vxe;
using namespace vxe::render;

TEST_CASE("Sun rays shine while the sun is in view and fade as it leaves") {
    Camera camera; // Looks along -Z, level.
    const f32 aspect = 16.0F / 9.0F;
    // Straight ahead, a little above the horizon: the middle of the screen, a bit up.
    const GodRayView ahead = godRayView(camera, aspect, glm::normalize(Vec3(0.0F, 0.15F, -1.0F)));
    CHECK(ahead.sunUv.x == doctest::Approx(0.5F));
    CHECK(ahead.sunUv.y > 0.5F);
    CHECK(ahead.sunUv.y < 1.0F);
    CHECK(ahead.strength > 0.5F);
    // Behind the camera: none.
    CHECK(godRayView(camera, aspect, glm::normalize(Vec3(0.0F, 0.15F, 1.0F))).strength == 0.0F);
    // Below the horizon: none.
    CHECK(godRayView(camera, aspect, glm::normalize(Vec3(0.0F, -0.2F, -1.0F))).strength == 0.0F);
    // Far to the side, off the screen: none.
    CHECK(godRayView(camera, aspect, glm::normalize(Vec3(1.0F, 0.1F, -0.2F))).strength ==
          doctest::Approx(0.0F));
}
