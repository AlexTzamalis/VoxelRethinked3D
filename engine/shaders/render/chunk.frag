// Chunk quads (render::ChunkRenderer): the block texture times the face
// shading. Built three times: plain for opaque blocks, with
// VXE_CHUNK_CUTOUT for alpha tested ones (leaves) and with
// VXE_CHUNK_TRANSLUCENT for blended ones (water, glass).
#include "include/bindings.glsl"

VXE_BINDING(2, 2) uniform sampler2DArray blockTextures;

layout(location = 0) in vec3 vUvLayer;
layout(location = 1) in float vShade;
layout(location = 0) out vec4 outColor;

void main() {
    const vec4 albedo = texture(blockTextures, vUvLayer);
#ifdef VXE_CHUNK_CUTOUT
    if (albedo.a < 0.5) {
        discard;
    }
#endif
#ifdef VXE_CHUNK_TRANSLUCENT
    outColor = vec4(albedo.rgb * vShade, albedo.a);
#else
    outColor = vec4(albedo.rgb * vShade, 1.0);
#endif
}
