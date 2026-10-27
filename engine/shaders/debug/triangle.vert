// A colored triangle with no vertex buffer: positions come from
// gl_VertexID, the same vertex pulling idea the chunk renderer uses
// (Section 8.2). Used by the RHI smoke tests to prove a draw works.
#include "include/bindings.glsl"

VXE_BINDING_STD140(1, 0) uniform TriangleParams {
    vec4 tint;   // Multiplied with the vertex colors.
    vec2 offset; // Clip space offset, so tests can move the triangle.
    float scale;
    float depth; // Clip space depth of every vertex, for depth tests.
} params;

layout(location = 0) out vec3 vColor;

const vec2 kCorners[3] = vec2[3](vec2(-0.6, -0.5), vec2(0.6, -0.5), vec2(0.0, 0.6));
const vec3 kColors[3] = vec3[3](vec3(1.0, 0.25, 0.2), vec3(0.2, 0.9, 0.3), vec3(0.25, 0.4, 1.0));

void main() {
    vColor = kColors[gl_VertexID] * params.tint.rgb;
    gl_Position = vec4(kCorners[gl_VertexID] * params.scale + params.offset, params.depth, 1.0);
}
