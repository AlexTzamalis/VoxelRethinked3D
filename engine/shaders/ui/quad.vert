// Game UI quads (E12.1), pulled from a storage buffer: six vertices per
// quad, two triangles, no index buffer. Positions are window pixels with the
// origin at the top left (vxe/ui/ui_renderer.hpp has the Quad layout).
#include "include/bindings.glsl"

struct Quad {
    vec4 rect; // x, y, width, height
    vec4 uv;   // u0, v0, u1, v1
    uint color;
    uint flags;
    uint padding0;
    uint padding1;
};

VXE_BINDING_STD430(2, 0) readonly buffer UiQuads {
    Quad quads[];
} ui;

// Maps window pixels (y down) to clip space (y up).
VXE_BINDING_STD140(1, 0) uniform UiProjection {
    vec2 scale;
    vec2 translate;
} projection;

layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec4 vColor;
layout(location = 2) flat out uint vFlags;

// The corners of the two triangles: (0,0) (1,0) (1,1), (0,0) (1,1) (0,1).
const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                 vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    const Quad quad = ui.quads[gl_VertexID / 6];
    const vec2 corner = kCorners[gl_VertexID % 6];
    const vec2 position = quad.rect.xy + corner * quad.rect.zw;
    vUv = mix(quad.uv.xy, quad.uv.zw, corner);
    vColor = unpackUnorm4x8(quad.color);
    vFlags = quad.flags;
    gl_Position = vec4(position * projection.scale + projection.translate, 0.0, 1.0);
}
