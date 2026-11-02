#pragma once
// The sky's colors and the height fog (render::SkyRenderer), shared by the
// sky and fog pass and by translucent chunks, which are drawn after that pass
// and fog themselves the same way (otherwise distant water stays clear while
// the far terrain next to it is fogged). The layout must match
// render::SkyParams. A shader picks the slot in set 1 with VXE_SKY_SLOT
// (default 0) before including this file.
#include "include/bindings.glsl"
#include "include/frame.glsl"

#ifndef VXE_SKY_SLOT
#define VXE_SKY_SLOT 0
#endif

VXE_BINDING_STD140(1, VXE_SKY_SLOT) uniform SkyParams {
    vec4 sunDirection; // xyz: towards the sun, w: cosine of the sun disk radius.
    vec4 zenithColor;
    vec4 horizonColor;
    vec4 sunColor;     // rgb: sun disk and glow color, a: glow strength.
    vec4 fog;          // start (blocks), density (per block), height falloff (per block), base height (world y).
    ivec4 flags;       // x: sky on, y: fog on, z: sun and moon images loaded, w: the moon's image.
    vec4 moonDirection; // xyz: towards the moon, w: half its size (tangent).
    vec4 moonColor;     // rgb: tint, a: how much of it shows.
    vec4 skyLight;      // rgb: the color sky light gives terrain.
    vec4 stars;         // xyz: the axis the night sky turns around, w: its angle.
    vec4 night;         // x: how strong stars are, y: how strong the galaxy is.
} sky;

vec3 skyColor(vec3 direction) {
    // Gradient: horizon color at and below the horizon, zenith color above,
    // with a curve that keeps a band of horizon haze.
    const float height = clamp(direction.y, 0.0, 1.0);
    vec3 color = mix(sky.horizonColor.rgb, sky.zenithColor.rgb, pow(height, 0.45));
    // Below the horizon (where terrain will normally be) the haze darkens
    // towards a ground tone, so empty space under the horizon does not look
    // like a second sky.
    color = mix(color, sky.horizonColor.rgb * 0.45, smoothstep(0.0, 0.35, -direction.y));
    const float towardsSun = dot(direction, sky.sunDirection.xyz);
    // Glow: wide and soft, strongest near the horizon (longer path through air).
    color += sky.sunColor.rgb * sky.sunColor.a * pow(max(towardsSun, 0.0), 8.0) * (1.0 - 0.6 * height);
    return color;
}

// The fraction of light lost to fog along a ray from the camera, for fog
// whose density falls off exponentially with height. The integral of
// density * exp(-falloff * (y - base)) along the ray has a closed form, so no
// marching is needed; the small slope case uses its limit to avoid 0 / 0.
float heightFog(float cameraY, vec3 direction, float distance) {
    const float start = sky.fog.x;
    const float density = sky.fog.y;
    const float falloff = sky.fog.z;
    const float base = sky.fog.w;
    const float travelled = max(distance - start, 0.0);
    if (travelled <= 0.0) {
        return 0.0;
    }
    const float startY = cameraY + direction.y * start - base;
    const float slope = falloff * direction.y * travelled;
    const float shape = abs(slope) > 1e-4 ? (1.0 - exp(-slope)) / slope : 1.0;
    const float opticalDepth = density * travelled * exp(-falloff * startY) * shape;
    return clamp(1.0 - exp(-opticalDepth), 0.0, 1.0);
}

// The fog color seen in a direction: the sky's color near the horizon there,
// so far terrain fades into exactly the sky behind it.
vec3 fogColor(vec3 direction) {
    return skyColor(normalize(vec3(direction.x, max(direction.y, 0.0) * 0.25, direction.z)));
}

// The camera's height in world blocks.
float cameraWorldY() {
    return float(frame.cameraChunk.y * 16) + frame.cameraFraction.y;
}
