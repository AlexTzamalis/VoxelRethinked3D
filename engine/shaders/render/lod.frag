// Far terrain (render::LodRenderer): each material's average color with the
// detail of its block texture, discarded over chunk columns that real chunks
// draw (the coverage map, Section 13.5).
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD140(1, 0) uniform LodParams {
    vec4 params; // x: daylight, y: darkest brightness, z: coverage (0 off, 1 discard, 2 show), w: flags (lod.vert).
    vec4 skyLight; // rgb: the color of sky light right now.
} lodParams;

// One texel per chunk column; columns wrap around the map.
VXE_BINDING(2, 3) uniform sampler2D coverage;
const int kCoverageSize = 256; // render::kLodCoverageSize.

layout(location = 0) in vec3 vColor;
// The chunks' block textures. Materials below the layer count are their layers.
VXE_BINDING(2, 4) uniform sampler2DArray blockTextures;

layout(location = 1) in vec3 vRelative;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in uint vMaterial;
layout(location = 0) out vec4 outColor;

// The texture's light and dark against its own average, not the texture itself:
// the color stays the material's, which holds the block's tint and overlays, so
// far terrain keeps the chunks' colors. Further away the mipmaps run towards that
// same average and the detail fades to nothing, so it cannot start a seam.
vec3 textureDetail() {
    const uint flags = uint(lodParams.params.w);
    if ((flags & 4u) == 0u || vMaterial >= (flags >> 3u)) {
        return vec3(1.0);
    }
    const vec3 at = vec3(vUv, float(vMaterial));
    const vec4 texel = texture(blockTextures, at);
    const vec3 average = textureLod(blockTextures, at, 16.0).rgb;
    return mix(vec3(1.0), clamp(texel.rgb / max(average, vec3(0.02)), vec3(0.0), vec3(3.0)), texel.a);
}

void main() {
    if (lodParams.params.z > 0.5) {
        // The chunk column, with integer math from the camera's chunk.
        const ivec2 offset = ivec2(floor((vRelative.xz + frame.cameraFraction.xz) / 16.0));
        const ivec2 column = (frame.cameraChunk.xz + offset) & ivec2(kCoverageSize - 1);
        // 0 is uncovered, 255 fully covered, anything else the top chunk layer
        // of the real chunks plus 65: far terrain goes on above that.
        const float value = texelFetch(coverage, column, 0).r * 255.0;
        // The map repeats, so far ground lands on the texels of columns near the
        // camera. Only columns within half the map are really those columns;
        // without this the shape of the loaded chunks shows as holes further out.
        const bool near = max(abs(offset.x), abs(offset.y)) < kCoverageSize / 2;
        bool hidden = near && value > 254.5;
        if (near && !hidden && value > 0.5) {
            const int layer = frame.cameraChunk.y + int(floor((vRelative.y + frame.cameraFraction.y) / 16.0));
            hidden = layer <= int(value + 0.5) - 65;
        }
        if (hidden) {
            if (lodParams.params.z > 1.5) {
                outColor = vec4(1.0, 0.0, 1.0, 1.0); // Debug.LOD.ShowCoverage
                return;
            }
            discard;
        }
    }
    outColor = vec4(vColor * textureDetail(), 1.0);
}
