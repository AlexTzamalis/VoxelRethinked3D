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
layout(location = 0) out vec4 outColor;

void main() {
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
    vec3 color = albedo.rgb * vShade;
    if (sky.flags.y != 0) {
        const float distance = length(vRelative);
        const vec3 direction = vRelative / max(distance, 1e-4);
        color = mix(color, fogColor(direction), heightFog(cameraWorldY(), direction, distance));
    }
    outColor = vec4(color, albedo.a);
#else
    // Wet ground is darker, since water fills the gaps that scattered light, and
    // a wet top shines with the sky when seen low across it.
    vec3 color = albedo.rgb * vShade * (1.0 - vWet * 0.38);
    const float grazing = 1.0 - abs(vRelative.y) / max(length(vRelative), 1e-4);
    color += vSheen * pow(grazing, 4.0);
    outColor = vec4(color, 1.0);
#endif
}
