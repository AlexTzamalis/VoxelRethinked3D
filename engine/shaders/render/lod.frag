// Far terrain (render::LodRenderer): flat colors, discarded over chunk
// columns that real chunks draw (the coverage map, Section 13.5).
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD140(1, 0) uniform LodParams {
    vec4 params; // x: daylight, y: darkest brightness, z: coverage (0 off, 1 discard, 2 show), w: color by level.
} lodParams;

// One texel per chunk column; columns wrap around the 128 x 128 map.
VXE_BINDING(2, 3) uniform sampler2D coverage;

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec3 vRelative;
layout(location = 0) out vec4 outColor;

void main() {
    if (lodParams.params.z > 0.5) {
        // The chunk column, with integer math from the camera's chunk.
        const ivec2 offset = ivec2(floor((vRelative.xz + frame.cameraFraction.xz) / 16.0));
        const ivec2 column = (frame.cameraChunk.xz + offset) & ivec2(127);
        if (texelFetch(coverage, column, 0).r > 0.5) {
            if (lodParams.params.z > 1.5) {
                outColor = vec4(1.0, 0.0, 1.0, 1.0); // Debug.LOD.ShowCoverage
                return;
            }
            discard;
        }
    }
    outColor = vec4(vColor, 1.0);
}
