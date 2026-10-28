// Debug lines (render::DebugDraw), pulled from a storage buffer: two
// vertices per line, each a camera relative position (computed on the CPU in
// double, so lines stay steady far from the origin) and a packed RGBA8 color.
#include "include/bindings.glsl"
#include "include/frame.glsl"

struct LineVertex {
    vec3 position;
    uint color;
};

VXE_BINDING_STD430(2, 0) readonly buffer LineVertices {
    LineVertex vertices[];
};

layout(location = 0) out vec4 vColor;

void main() {
    const LineVertex vertex = vertices[gl_VertexID];
    vColor = unpackUnorm4x8(vertex.color);
    gl_Position = frame.viewProjection * vec4(vertex.position, 1.0);
}
