#include "vxe/render/shadows.hpp"

#include <algorithm>
#include <cmath>

namespace vxe::render {
namespace {

using QualityOption = config::EnumSettingDesc<ShadowQuality>::Option;
constexpr std::array<QualityOption, 3> kQualities{{
    {.value = ShadowQuality::Low, .name = "Low", .displayName = "Low (2 cascades, hard edges)"},
    {.value = ShadowQuality::Medium, .name = "Medium", .displayName = "Medium (3 cascades, soft edges)"},
    {.value = ShadowQuality::High, .name = "High", .displayName = "High (4 cascades, softest edges)"},
}};

f32 smoothstep(f32 low, f32 high, f32 value) noexcept {
    const f32 t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

} // namespace

ShadowSettings::ShadowSettings(config::SettingsRegistry& settings)
    : m_enabled(
          settings.registerBool({{.id = "Graphics.Shadows.Enabled",
                                  .displayName = "Shadows",
                                  .description = "The sun and the moon cast shadows: trees on the ground, "
                                                 "mountains on valleys."},
                                 true})) {
    config::EnumSettingDesc<ShadowQuality> quality;
    quality.id = "Graphics.Shadows.Quality";
    quality.displayName = "Shadow quality";
    quality.description = "More cascades keep shadows sharp further away; softer edges cost more.";
    quality.defaultValue = ShadowQuality::Medium;
    quality.options = kQualities;
    m_quality = settings.registerEnum(quality);
    m_distance =
        settings.registerInt({{.id = "Graphics.Shadows.Distance",
                               .displayName = "Shadow distance",
                               .description = "Blocks from the camera that shadows reach. Further away "
                                              "the terrain is unshadowed."},
                              64,
                              32,
                              256,
                              16});
}

ShadowFrame computeShadowFrame(const Camera& camera, f32 aspect, const Vec3& toLight, f32 strength,
                               u32 cascades, f32 distance) noexcept {
    ShadowFrame frame;
    frame.cascades = std::clamp<u32>(cascades, 1, kShadowMaxCascades);
    frame.toLight = glm::normalize(toLight);
    frame.strength = strength;
    frame.enabled = strength > 0.0F;

    // The light's axes: Z towards the light, X and Y across the map.
    const glm::dvec3 z(frame.toLight);
    const glm::dvec3 up = std::abs(z.y) > 0.99 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
    const glm::dvec3 x = glm::normalize(glm::cross(up, z));
    const glm::dvec3 y = glm::cross(z, x);

    const f64 tanY = std::tan(glm::radians(static_cast<f64>(camera.fovYDegrees)) * 0.5);
    const f64 tanX = tanY * static_cast<f64>(aspect);
    const f64 spread = (tanX * tanX) + (tanY * tanY); // Squared slope to a corner of the view.
    const glm::dvec3 forward(camera.forward());
    f64 near = 0.0;
    for (u32 i = 0; i < frame.cascades; ++i) {
        // Between even and quadratic steps: more of the map close to the camera.
        const f64 share = static_cast<f64>(i + 1) / static_cast<f64>(frame.cascades);
        const f64 far = static_cast<f64>(distance) * ((0.5 * share) + (0.5 * share * share));
        // The smallest sphere around this slice of the view: its center sits
        // further along than the middle, since the far end is wider.
        const f64 along = std::min(far, 0.5 * (far + near) * (1.0 + spread));
        const f64 farRadius = std::sqrt(((far - along) * (far - along)) + (far * far * spread));
        const f64 nearRadius = std::sqrt(((along - near) * (along - near)) + (near * near * spread));
        // Whole texels of a fixed size, so the radius never jitters either.
        const f64 radius = std::ceil(std::max(farRadius, nearRadius) + 1.0);
        const f64 texel = (2.0 * radius) / static_cast<f64>(kShadowTileSize);
        const glm::dvec3 world = camera.position + (forward * along);
        const f64 lx = glm::dot(world, x);
        const f64 ly = glm::dot(world, y);
        const glm::dvec3 snapped = world + (x * ((std::floor(lx / texel) * texel) - lx)) +
                                   (y * ((std::floor(ly / texel) * texel) - ly));
        const Vec3 center(snapped - camera.position);
        const auto r = static_cast<f32>(radius);
        const f32 range = (2.0F * r) + kShadowCasterReach;

        ShadowCascade& cascade = frame.cascade.at(i);
        cascade.center = center;
        cascade.radius = r;
        cascade.depthRange = range;
        cascade.far = static_cast<f32>(far);
        const Vec3 fx(x);
        const Vec3 fy(y);
        const Vec3 fz(z);
        Mat4 m(0.0F);
        for (int c = 0; c < 3; ++c) {
            m[c][0] = fx[c] / r;
            m[c][1] = fy[c] / r;
            m[c][2] = fz[c] / range;
        }
        m[3][0] = -glm::dot(fx, center) / r;
        m[3][1] = -glm::dot(fy, center) / r;
        // Depth 0 at the far side of the sphere, 1 at kShadowCasterReach
        // past its near side towards the light (reversed Z, like the scene).
        m[3][2] = (r - glm::dot(fz, center)) / range;
        m[3][3] = 1.0F;
        cascade.matrix = m;
        near = far;
    }
    return frame;
}

ShadowLight shadowLightOf(const Vec3& toSun, const Vec3& toMoon) noexcept {
    // Long shadows at a low sun flicker and stretch, so they fade in over the
    // first ten degrees or so; at night the moon casts faint ones.
    if (toSun.y > 0.0F) {
        return {.toLight = toSun, .strength = smoothstep(0.03F, 0.2F, toSun.y)};
    }
    return {.toLight = toMoon, .strength = 0.5F * smoothstep(0.03F, 0.2F, toMoon.y)};
}

} // namespace vxe::render
