// Dear ImGui vertices, pulled from a storage buffer (Section 8.2's vertex
// pulling, also used here so the RHI needs no vertex input layouts).
//
// An ImDrawVert is 20 bytes: position (2 floats), uv (2 floats), color (4
// bytes). A std430 struct with those members would be padded to 24 bytes, so
// the buffer is read as plain 32 bit words and each vertex starts at word
// index * 5. gl_VertexID already includes the draw's base vertex.
#include "include/bindings.glsl"

VXE_BINDING_STD430(2, 0) readonly buffer ImGuiVertices {
    uint words[];
} vertices;

// Maps ImGui's pixel coordinates (origin top left, y down) to clip space.
VXE_BINDING_STD140(1, 0) uniform ImGuiProjection {
    vec2 scale;
    vec2 translate;
} projection;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main() {
    const uint base = uint(gl_VertexID) * 5u;
    const vec2 position = vec2(uintBitsToFloat(vertices.words[base]), uintBitsToFloat(vertices.words[base + 1u]));
    vUv = vec2(uintBitsToFloat(vertices.words[base + 2u]), uintBitsToFloat(vertices.words[base + 3u]));
    // ImGui packs colors as bytes R, G, B, A from the lowest byte up, which is
    // exactly the order unpackUnorm4x8 reads.
    vColor = unpackUnorm4x8(vertices.words[base + 4u]);
    gl_Position = vec4(position * projection.scale + projection.translate, 0.0, 1.0);
}
