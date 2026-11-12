#pragma once
// Long shadows (render::SunlightShadows): mountains and far terrain shading the
// land on the far side from the sun, and clouds shading the ground under them.
// Both work beyond the shadow map's few dozen blocks, at any distance far
// terrain draws. The layout must match render::SunlightParams.
#include "include/bindings.glsl"

VXE_BINDING_STD140(1, 5) uniform SunlightParams {
    vec4 terrain; // xy: corner of the height grid from the camera, z: blocks per cell, w: cells (0 off).
    vec4 heights; // x: the camera's world height, y: bias, z: softness, w: strength (0 off).
    vec4 cloud;   // x: floor (world y), y: map value where cloud starts, z: strength (0 off), w: thickness.
    vec4 cloudAt; // xy: the camera in the map's repeating space, wind included.
    vec4 toLight; // xyz: towards the sun or moon.
} sunlight;

// The height up to which each column of the grid lies in the shadow of the
// land between it and the light (render/terrain_shadow.comp).
VXE_BINDING(2, 6) uniform sampler2D terrainShadow;
// The clouds' density map (render::CloudRenderer), repeating.
VXE_BINDING(2, 7) uniform sampler2D sunlightCloudMap;

const float kSunlightCloudMapBlocks = 4096.0; // render::kCloudMapSize * kCloudCellBlocks.

// 1 where the land towards the light leaves this camera relative point in the
// open, 0 where a mountain stands in the way, with soft edges.
float terrainSunlit(vec3 relative) {
    if (sunlight.terrain.w <= 0.0 || sunlight.heights.w <= 0.0) {
        return 1.0;
    }
    const vec2 uv = (relative.xz - sunlight.terrain.xy) / (sunlight.terrain.z * sunlight.terrain.w);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    const float shadowTop = textureLod(terrainShadow, uv, 0.0).r - sunlight.heights.x - sunlight.heights.y;
    const float lit = smoothstep(shadowTop - sunlight.heights.z, shadowTop, relative.y);
    return mix(1.0, lit, sunlight.heights.w);
}

// 1 under open sky, less under a cloud between this point and the light.
float cloudSunlit(vec3 relative) {
    const vec3 toLight = sunlight.toLight.xyz;
    if (sunlight.cloud.z <= 0.0 || toLight.y < 0.05) {
        return 1.0;
    }
    // Where the line to the light crosses the middle of the cloud layer.
    const float middle = sunlight.cloud.x + sunlight.cloud.w * 0.35;
    const float up = middle - (relative.y + sunlight.heights.x);
    if (up <= 0.0) {
        return 1.0;
    }
    const vec2 at = sunlight.cloudAt.xy + relative.xz + toLight.xz * (up / toLight.y);
    const float map = textureLod(sunlightCloudMap, at / kSunlightCloudMapBlocks, 1.0).r;
    const float cover = smoothstep(sunlight.cloud.y, sunlight.cloud.y + 0.12, map);
    return 1.0 - cover * sunlight.cloud.z;
}

// Both at once.
float farSunlit(vec3 relative) {
    return terrainSunlit(relative) * cloudSunlit(relative);
}
