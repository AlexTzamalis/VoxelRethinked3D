// A cube from 36 indices into its 8 corners, with no vertex buffer. The
// index value arrives as gl_VertexID, and its three low bits are the corner
// (bit 0 is x, bit 1 is y, bit 2 is z). Used by the RHI smoke tests and the
// RHI demo to prove indexed draws, depth testing and reversed Z.
#include "include/bindings.glsl"

VXE_BINDING_STD140(1, 1) uniform CubeParams {
    mat4 modelViewProjection;
    mat4 model;
} params;

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vObjectPosition;

void main() {
    const vec3 corner = vec3(gl_VertexID & 1, (gl_VertexID >> 1) & 1, (gl_VertexID >> 2) & 1) * 2.0 - 1.0;
    vWorldPosition = (params.model * vec4(corner, 1.0)).xyz;
    vObjectPosition = corner;
    gl_Position = params.modelViewProjection * vec4(corner, 1.0);
}
