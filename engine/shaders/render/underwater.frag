// Fog under water (render::WaterRenderer): with the eye in water, the scene
// fades into the water's color with distance, and open sky (depth 0, ADR-010)
// is all water. Blended over the scene by its amount.
#include "include/bindings.glsl"
#include "include/frame.glsl"

VXE_BINDING_STD140(1, 4) uniform WaterParams {
    vec4 fog;     // rgb: the fog's color, a: its density per block.
    vec4 surface; // Read by liquid surfaces (chunk.frag).
} water;

VXE_BINDING(2, 0) uniform sampler2D sceneDepth;

layout(location = 0) in vec2 vClip;
layout(location = 0) out vec4 outColor;

void main() {
    const float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float distance = 1.0e4;
    if (depth > 0.0) {
        const vec4 point = frame.inverseViewProjection * vec4(vClip, depth, 1.0);
        distance = length(point.xyz / point.w);
    }
    const float amount = 1.0 - exp(-distance * water.fog.a);
    outColor = vec4(water.fog.rgb, amount);
}
