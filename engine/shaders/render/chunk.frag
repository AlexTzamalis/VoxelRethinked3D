// Chunk quads (render::ChunkRenderer): the block texture with its tint and
// overlay (voxel::textureLook), times the face shading. Built three times: plain for opaque blocks, with
// VXE_CHUNK_CUTOUT for alpha tested ones (leaves) and with
// VXE_CHUNK_TRANSLUCENT for blended ones (water, glass).
#include "include/bindings.glsl"
#ifdef VXE_CHUNK_TRANSLUCENT
// Translucent chunks come after the sky and fog pass, which fogs only what
// wrote depth, so they fog themselves (sky parameters in set 1, slot 1).
#define VXE_SKY_SLOT 1
#include "include/sky.glsl"
#include "include/frame.glsl"

// Water (render::WaterRenderer): how much light it swallows behind its
// surface, and the depth of the solid scene behind it.
VXE_BINDING_STD140(1, 4) uniform WaterParams {
    vec4 fog;     // Used under water (underwater.frag).
    vec4 surface; // x: light swallowed per block (0 off), y: share of the color left in the deep.
} water;
VXE_BINDING(2, 8) uniform sampler2D solidDepth;
#endif

VXE_BINDING(2, 2) uniform sampler2DArray blockTextures;

layout(location = 0) in vec3 vUvLayer;
layout(location = 1) in vec3 vShade;
layout(location = 2) in vec3 vRelative;
layout(location = 3) flat in vec3 vTint;
layout(location = 4) flat in vec3 vOverlayTint;
layout(location = 5) flat in uint vOverlayLayer;
layout(location = 6) in float vWet;
layout(location = 7) flat in vec3 vSheen;
layout(location = 8) in vec3 vSky;
layout(location = 9) in vec3 vBlock;
layout(location = 10) in float vBase;
layout(location = 11) flat in vec3 vNormal;
layout(location = 12) flat in uint vLiquid;

#ifndef VXE_CHUNK_SHADOW
// Mountain and cloud shadows beyond the shadow map (render::SunlightShadows).
#include "include/sunlight.glsl"

VXE_BINDING_STD140(1, 0) uniform ChunkParams {
    vec4 params; // z: darkest brightness (as chunk.vert).
    vec4 skyLight;
    vec4 tints[16];
} chunkParams;

// Sun and moon shadows (render::ChunkRenderer, render::ShadowFrame).
VXE_BINDING_STD140(1, 2) uniform ShadowParams {
    mat4 matrices[4];
    vec4 splits;     // Where each cascade ends, in blocks from the camera.
    vec4 depthRange; // Blocks across each cascade's depth.
    vec4 radius;     // Half the width of each cascade.
    vec4 toLight;    // xyz towards the light, w strength (0 is off).
    vec4 settings;   // x cascades, y filter radius in texels, w the shadow distance.
} shadow;
VXE_BINDING(2, 5) uniform sampler2D shadowMap;

const float kShadowAtlas = 4096.0; // render::kShadowAtlasSize.
// What is left of sky light in shadow: the open sky still lights it.
const float kShadowAmbient = 0.55;

// How much of the light reaches this pixel, 0 (in shadow) to 1.
float sunlit() {
    const float distance = length(vRelative);
    const int cascades = int(shadow.settings.x);
    int c = -1;
    for (int i = 0; i < cascades; ++i) {
        if (distance < shadow.splits[i]) {
            c = i;
            break;
        }
    }
    if (c < 0) {
        return 1.0;
    }
    const bool plant = vNormal == vec3(0.0);
    // A face turned away from the light is in its own shadow.
    if (!plant && dot(vNormal, shadow.toLight.xyz) <= 0.0) {
        return 0.0;
    }
    // Lifted off the face by about a texel, so it never shades itself.
    const float texel = 2.0 * shadow.radius[c] / (kShadowAtlas * 0.5);
    const vec3 position = vRelative + (plant ? shadow.toLight.xyz * texel * 3.0 : vNormal * texel * 1.5);
    const vec4 light = shadow.matrices[c] * vec4(position, 1.0);
    const vec2 uv = (light.xy * 0.5) + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return 1.0;
    }
    const vec2 tile = vec2(float(c % 2), float(c / 2));
    const float bias = (plant ? 0.3 : 0.04) / shadow.depthRange[c];
    const int radius = int(shadow.settings.y);
    const vec2 lowest = (tile + vec2(0.5 / (kShadowAtlas * 0.5))) * 0.5;
    const vec2 highest = (tile + vec2(1.0 - (0.5 / (kShadowAtlas * 0.5)))) * 0.5;
    float lit = 0.0;
    float taps = 0.0;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const vec2 at = clamp(((uv + tile) * 0.5) + (vec2(x, y) / kShadowAtlas), lowest, highest);
            lit += texture(shadowMap, at).r > light.z + bias ? 0.0 : 1.0;
            taps += 1.0;
        }
    }
    // Fades out towards the shadow distance, where far terrain has none.
    const float fade = smoothstep(shadow.settings.w * 0.8, shadow.settings.w, distance);
    return mix(lit / taps, 1.0, fade);
}

// vShade with the sky light shaded by the shadow map near the camera, and by
// mountains and clouds at any distance.
vec3 shadedLight() {
    const float farLit = farSunlit(vRelative);
    if (shadow.toLight.w <= 0.0 && farLit >= 1.0) {
        return vShade;
    }
    const float nearLit = shadow.toLight.w > 0.0 ? sunlit() : 1.0;
    const float keep = min(1.0 - (shadow.toLight.w * (1.0 - nearLit) * (1.0 - kShadowAmbient)),
                           1.0 - ((1.0 - farLit) * (1.0 - kShadowAmbient)));
    const vec3 light = mix(vec3(chunkParams.params.z), vec3(1.0), max(vSky * keep, vBlock));
    return vBase * light;
}
#endif
layout(location = 0) out vec4 outColor;

void main() {
#ifdef VXE_CHUNK_SHADOW
    // Into the shadow map: depth only, with the holes of leaves and plants.
#ifdef VXE_CHUNK_CUTOUT
    if (texture(blockTextures, vUvLayer).a < 0.5) {
        discard;
    }
#endif
    return;
#else
    const vec3 shade = shadedLight();
    vec4 albedo = texture(blockTextures, vUvLayer);
    albedo.rgb *= vTint;
    if (vOverlayLayer != 0u) {
        const vec4 overlay = texture(blockTextures, vec3(vUvLayer.xy, float(vOverlayLayer - 1u)));
        albedo.rgb = mix(albedo.rgb, overlay.rgb * vOverlayTint, overlay.a);
        albedo.a = max(albedo.a, overlay.a);
    }
#ifdef VXE_CHUNK_CUTOUT
    if (albedo.a < 0.5) {
        discard;
    }
#endif
#ifdef VXE_CHUNK_TRANSLUCENT
    vec3 color = albedo.rgb * shade;
    if (vLiquid != 0u && water.surface.x > 0.0) {
        // The deeper the water behind the surface, the more of the light from
        // the floor it swallows: clear over a shallow floor, its own darker
        // color and nearly opaque over the deep. Open sky behind counts as deep.
        const float behind = texelFetch(solidDepth, ivec2(gl_FragCoord.xy), 0).r;
        float thickness = 64.0;
        if (behind > 0.0) {
            const vec2 clip = gl_FragCoord.xy * frame.viewport.zw * 2.0 - 1.0;
            const vec4 point = frame.inverseViewProjection * vec4(clip, behind, 1.0);
            thickness = max(length(point.xyz / point.w) - length(vRelative), 0.0);
        }
        const float deep = 1.0 - exp(-thickness * water.surface.x);
        color = mix(color, color * water.surface.y, deep);
        albedo.a = mix(albedo.a * 0.55, 0.96, deep);
    }
    if (sky.flags.y != 0) {
        const float distance = length(vRelative);
        const vec3 direction = vRelative / max(distance, 1e-4);
        color = mix(color, fogColor(direction), heightFog(cameraWorldY(), direction, distance));
    }
    outColor = vec4(color, albedo.a);
#else
    // Wet ground is darker, since water fills the gaps that scattered light, and
    // a wet top shines with the sky when seen low across it.
    vec3 color = albedo.rgb * shade * (1.0 - vWet * 0.38);
    const float grazing = 1.0 - abs(vRelative.y) / max(length(vRelative), 1e-4);
    color += vSheen * pow(grazing, 4.0);
    outColor = vec4(color, 1.0);
#endif
#endif
}
