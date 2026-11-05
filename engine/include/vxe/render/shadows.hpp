#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/core/types.hpp>
#include <vxe/math/vector.hpp>
#include <vxe/render/camera.hpp>

#include <array>

namespace vxe::render {

// Sun and moon shadows from a shadow map: terrain is drawn from the light's
// direction into one depth texture split into a 2 by 2 grid of cascades. Near
// the camera a cascade covers a small area at high detail; each next one
// covers more ground at less detail, out to Graphics.Shadows.Distance. The
// chunk shader compares each pixel's depth from the light with the map and
// darkens sky light where something stood between.
inline constexpr u32 kShadowMaxCascades = 4;
inline constexpr u32 kShadowAtlasSize = 4096; // Four tiles of 2048.
inline constexpr u32 kShadowTileSize = kShadowAtlasSize / 2;
// Blocks of terrain towards the light that can still cast into a cascade:
// mountains far off on the sun's side.
inline constexpr f32 kShadowCasterReach = 160.0F;

// One cascade: how camera relative positions map into its tile of the atlas.
struct ShadowCascade {
    // Camera relative position to clip space: x and y -1 to 1 across the
    // tile, z 0 (far from the light) to 1 (nearest), as reversed Z.
    Mat4 matrix{1.0F};
    // Camera relative center and half size of the ground it covers (blocks).
    Vec3 center{0.0F};
    f32 radius = 0.0F;
    f32 depthRange = 1.0F; // Blocks from depth 0 to 1, for the bias.
    f32 far = 0.0F;        // Distance from the camera where the next cascade takes over.
};

struct ShadowFrame {
    bool enabled = false;
    u32 cascades = 0;
    Vec3 toLight{0.0F, 1.0F, 0.0F};
    f32 strength = 0.0F; // 0 no shadows (the light at the horizon) to 1.
    std::array<ShadowCascade, kShadowMaxCascades> cascade{};
};

// Cascades and edge softness.
enum class ShadowQuality : i32 {
    Low = 1,
    Medium = 2,
    High = 3,
};

// Graphics.Shadows.Enabled, Quality (cascades and softness) and Distance, all live.
class ShadowSettings {
public:
    explicit ShadowSettings(config::SettingsRegistry& settings);
    [[nodiscard]] bool enabled() const noexcept { return m_enabled.get(); }
    // 1 Low, 2 Medium, 3 High.
    [[nodiscard]] u32 quality() const noexcept { return static_cast<u32>(m_quality.get()); }
    [[nodiscard]] f32 distance() const noexcept { return static_cast<f32>(m_distance.get()); }
    [[nodiscard]] u32 cascades() const noexcept { return quality() + 1; }

private:
    config::Setting<bool> m_enabled;
    config::Setting<ShadowQuality> m_quality;
    config::Setting<i32> m_distance;
};

// The cascades for a camera. The light basis and each cascade's center are
// worked out in world space (double) and snapped to whole shadow map texels,
// so shadow edges stay still while the camera moves or turns instead of
// crawling. `toLight` points from the ground towards the sun or moon.
[[nodiscard]] ShadowFrame computeShadowFrame(const Camera& camera, f32 aspect, const Vec3& toLight,
                                             f32 strength, u32 cascades, f32 distance) noexcept;

// How strongly the sun or moon casts shadows: the one above the horizon, fading
// in as it rises. Shadows fade out towards sunset; the moon's are fainter.
struct ShadowLight {
    Vec3 toLight{0.0F, 1.0F, 0.0F};
    f32 strength = 0.0F;
};
[[nodiscard]] ShadowLight shadowLightOf(const Vec3& toSun, const Vec3& toMoon) noexcept;

} // namespace vxe::render
