// Far terrain (render::LodRenderer): flat colors, discarded over chunk
// columns that real chunks draw (the coverage map, Section 13.5).
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD140(1, 0) uniform LodParams {
    vec4 params; // x: daylight, y: darkest brightness, z: coverage (0 off, 1 discard, 2 show), w: flags (lod.vert).
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
        // 0 is uncovered, 255 fully covered, anything else the top chunk layer
        // of the real chunks plus 65: far terrain goes on above that.
        const float value = texelFetch(coverage, column, 0).r * 255.0;
        bool hidden = value > 254.5;
        if (!hidden && value > 0.5) {
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
    outColor = vec4(vColor, 1.0);
}
